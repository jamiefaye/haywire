#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Analyzing PGD[0] Kernel Mappings ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Read swapper_pg_dir
const pgdOffset = SWAPPER_PGD_PA - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, pgdOffset);

console.log(`Swapper PGD at PA: 0x${SWAPPER_PGD_PA.toString(16)}\n`);

// Analyze all PGD entries
console.log('PGD Entry Analysis:\n');

for (let i = 0; i < 512; i++) {
    const entry = pgdBuffer.readBigUint64LE(i * 8);
    if (entry !== 0n) {
        const valid = (entry & 0x3n) !== 0n;
        const tableAddr = Number(entry & PA_MASK & ~0xFFFn);
        const isBlock = (entry & 0x3n) === 0x1n;
        
        console.log(`PGD[${i}] = 0x${entry.toString(16)}`);
        
        // Calculate VA range this entry covers
        const vaStart = BigInt(i) << 39n;
        const vaEnd = ((BigInt(i) + 1n) << 39n) - 1n;
        
        console.log(`  VA range: 0x${vaStart.toString(16)} - 0x${vaEnd.toString(16)}`);
        
        if (valid) {
            if (isBlock) {
                console.log(`  1GB block mapping to PA 0x${tableAddr.toString(16)}`);
            } else {
                console.log(`  Points to table at PA 0x${tableAddr.toString(16)}`);
            }
        }
        
        // Special analysis for PGD[0]
        if (i === 0) {
            console.log('  \nPGD[0] Deep Dive:');
            console.log('  This typically maps user space (VA 0x0 - 0x8000000000)');
            console.log('  In swapper context, might have kernel direct mappings');
            
            if (valid && !isBlock) {
                // Read the PUD table
                const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
                fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, tableAddr - GUEST_RAM_START);
                
                console.log('  \n  Checking PUD entries in PGD[0]...');
                let mappedCount = 0;
                
                for (let j = 0; j < 512; j++) {
                    const pudEntry = pudBuffer.readBigUint64LE(j * 8);
                    if (pudEntry !== 0n) {
                        mappedCount++;
                        if (mappedCount <= 5) {  // Show first 5
                            const pudVaStart = (BigInt(i) << 39n) | (BigInt(j) << 30n);
                            console.log(`    PUD[${j}]: VA 0x${pudVaStart.toString(16)} = 0x${pudEntry.toString(16)}`);
                            
                            // Check if it's a block or table
                            if ((pudEntry & 0x3n) === 0x1n) {
                                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                                console.log(`      -> 1GB block to PA 0x${blockPA.toString(16)}`);
                            } else if ((pudEntry & 0x3n) === 0x3n) {
                                const tablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
                                console.log(`      -> Table at PA 0x${tablePA.toString(16)}`);
                            }
                        }
                    }
                }
                
                console.log(`  \n  Total PUD entries mapped: ${mappedCount}/512`);
            }
        }
        
        console.log('');
    }
}

console.log('\n=== Linear Mapping Analysis ===\n');

// Check if there's a linear mapping pattern
console.log('Checking for kernel linear mapping pattern...\n');

// For ARM64, typical linear mappings:
// 1. VA 0x0 - 0x100000000 -> PA 0x40000000 - 0x140000000 (4GB linear)
// 2. Or kernel might use high addresses only

// Test a few VAs to see if they map linearly
const testVAs = [
    { va: 0x0n, desc: 'VA 0x0' },
    { va: 0x40000000n, desc: 'VA 0x40000000' },
    { va: 0x80000000n, desc: 'VA 0x80000000' },
    { va: 0xC0000000n, desc: 'VA 0xC0000000' },
];

for (const test of testVAs) {
    console.log(`Testing ${test.desc}:`);
    
    // Which PGD entry?
    const pgdIndex = Number((test.va >> 39n) & 0x1FFn);
    const pudIndex = Number((test.va >> 30n) & 0x1FFn);
    
    if (pgdIndex !== 0) {
        console.log(`  Not in PGD[0] (would be in PGD[${pgdIndex}])`);
        continue;
    }
    
    const pgdEntry = pgdBuffer.readBigUint64LE(pgdIndex * 8);
    if ((pgdEntry & 0x3n) === 0n) {
        console.log(`  PGD[${pgdIndex}] is not valid`);
        continue;
    }
    
    if ((pgdEntry & 0x3n) === 0x1n) {
        // Block mapping
        const blockPA = Number(pgdEntry & PA_MASK & ~0x3FFFFFFFn);
        const offset = Number(test.va & 0x3FFFFFFFn);
        const pa = blockPA + offset;
        console.log(`  Maps via 1GB block to PA 0x${pa.toString(16)}`);
        
        // Check if it's linear
        const expectedPA = Number(test.va) + GUEST_RAM_START;
        if (pa === expectedPA) {
            console.log(`  ✓ LINEAR: VA + 0x${GUEST_RAM_START.toString(16)} = PA`);
        }
    } else {
        // Need to walk tables
        const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        
        const pudEntry = pudBuffer.readBigUint64LE(pudIndex * 8);
        if ((pudEntry & 0x3n) !== 0n) {
            console.log(`  Has mapping via PUD[${pudIndex}]`);
            
            if ((pudEntry & 0x3n) === 0x1n) {
                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                const offset = Number(test.va & 0x3FFFFFFFn);
                const pa = blockPA + offset;
                console.log(`  Maps to PA 0x${pa.toString(16)}`);
                
                // Check if it's linear
                const expectedPA = Number(test.va) + GUEST_RAM_START;
                if (pa === expectedPA) {
                    console.log(`  ✓ LINEAR: VA + 0x${GUEST_RAM_START.toString(16)} = PA`);
                }
            }
        } else {
            console.log(`  No mapping (PUD[${pudIndex}] is invalid)`);
        }
    }
    console.log('');
}

console.log('=== Summary ===\n');
console.log('PGD[0] typically contains:');
console.log('1. User space mappings (processes)');
console.log('2. In swapper context, may have kernel linear mappings');
console.log('3. Direct physical memory access for kernel');
console.log('');
console.log('For init_task at PA 0x37b39840:');
console.log('- This is at PA 0x37b39840 (below 0x40000000)');
console.log('- This is in LOW memory (below RAM start)');
console.log('- Likely in kernel code/rodata section');
console.log('- Not accessible via memory-backend-file');
console.log('');
console.log('To access init_task, we would need:');
console.log('1. QMP commands that read all physical memory');
console.log('2. Or a modified QEMU that includes kernel sections');
console.log('3. Or use the processes we CAN find (91% discovery)');

fs.closeSync(fd);