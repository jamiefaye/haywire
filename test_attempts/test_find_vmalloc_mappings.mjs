#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;

console.log('=== Finding VMALLOC Mappings in Page Tables ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Read swapper_pg_dir
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);

console.log('Kernel VA Layout (ARM64):\n');
console.log('Linear map:  0xFFFF800000000000 - 0xFFFF800180000000 (6GB of RAM)');
console.log('VMALLOC:     0xFFFF800008000000 - 0xFFFF800040000000 (typical)');
console.log('');
console.log('The vmalloc range OVERLAPS with linear map in PGD[256]!');
console.log('Let\'s examine the actual mappings...\n');

console.log('='.repeat(70) + '\n');

// Check PGD[256] and its PUD entries
const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
console.log(`PGD[256] entry: 0x${pgd256.toString(16)}`);

if (pgd256 !== 0n) {
    const pudTablePA = Number(pgd256 & PA_MASK & ~0xFFFn);
    console.log(`Points to PUD table at PA: 0x${pudTablePA.toString(16)}\n`);
    
    const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
    
    console.log('Scanning PUD entries in PGD[256]:\n');
    
    // The vmalloc area would be in higher PUD indices
    // Linear map starts at PUD[1] (0xFFFF800040000000)
    // VMALLOC might start around PUD[2] or higher
    
    const mappedPuds = [];
    for (let i = 0; i < 512; i++) {
        const pudEntry = pudBuffer.readBigUint64LE(i * 8);
        if (pudEntry !== 0n) {
            const vaStart = 0xFFFF800000000000n + (BigInt(i) << 30n);
            mappedPuds.push({ index: i, entry: pudEntry, vaStart });
        }
    }
    
    console.log(`Found ${mappedPuds.length} mapped PUD entries:\n`);
    
    for (const pud of mappedPuds) {
        console.log(`PUD[${pud.index}]: VA 0x${pud.vaStart.toString(16)}`);
        console.log(`  Entry: 0x${pud.entry.toString(16)}`);
        
        // Check if it's a block or table
        if ((pud.entry & 0x3n) === 0x1n) {
            console.log(`  Type: 1GB block (linear mapping)`);
        } else {
            console.log(`  Type: Page table`);
            
            // Let's sample this table to see what's mapped
            const pmdTablePA = Number(pud.entry & PA_MASK & ~0xFFFn);
            const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
            fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
            
            let mappedCount = 0;
            let hasNonLinear = false;
            
            for (let j = 0; j < 512; j++) {
                const pmdEntry = pmdBuffer.readBigUint64LE(j * 8);
                if (pmdEntry !== 0n) {
                    mappedCount++;
                    
                    // Check if this looks like a non-linear mapping
                    // Linear mappings have a predictable PA pattern
                    const expectedLinearPA = Number(pud.vaStart - 0xFFFF800000000000n + 0x40000000n + (BigInt(j) << 21n));
                    const actualPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                    
                    if (Math.abs(actualPA - expectedLinearPA) > 0x200000) {
                        hasNonLinear = true;
                    }
                }
            }
            
            console.log(`  Has ${mappedCount}/512 PMD entries`);
            if (hasNonLinear) {
                console.log(`  ⚠️ Contains NON-LINEAR mappings (likely vmalloc!)`);
            }
        }
        console.log('');
    }
}

console.log('='.repeat(70) + '\n');
console.log('Looking for task_struct signatures in vmalloc-like mappings...\n');

// Let's check if we can find task_structs with clean 9KB mappings
// They would appear in non-linear mapped regions

const pgd256Entry = pgdBuffer.readBigUint64LE(256 * 8);
if (pgd256Entry !== 0n) {
    const pudTablePA = Number(pgd256Entry & PA_MASK & ~0xFFFn);
    const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
    
    // Check PUD[2] which maps 0xFFFF800080000000+
    const pud2 = pudBuffer.readBigUint64LE(2 * 8);
    if (pud2 !== 0n) {
        console.log('Examining PUD[2] (VA 0xFFFF800080000000) for vmalloc mappings...');
        
        const pmdTablePA = Number(pud2 & PA_MASK & ~0xFFFn);
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        
        // Sample a few PMD entries
        console.log('\nSample PMD entries:');
        for (let i = 0; i < 10; i++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(i * 8);
            if (pmdEntry !== 0n) {
                const va = 0xFFFF800080000000n + (BigInt(i) << 21n);
                const pa = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                console.log(`  VA 0x${va.toString(16)} -> PA 0x${pa.toString(16)}`);
                
                // Check if this PA matches linear mapping expectation
                const expectedPA = Number(va - 0xFFFF800000000000n + 0x40000000n);
                if (pa !== expectedPA) {
                    console.log(`    ⚠️ Non-linear! (expected PA 0x${expectedPA.toString(16)})`);
                    console.log(`    This could be a vmalloc mapping!`);
                }
            }
        }
    }
}

console.log('\n' + '='.repeat(70) + '\n');
console.log('The Challenge:\n');
console.log('1. VMALLOC mappings ARE in PGD[256] page tables');
console.log('2. But they\'re SPARSE - most VAs are unmapped');
console.log('3. We don\'t know which VAs have task_structs');
console.log('4. Would need to scan ALL page table entries');
console.log('');
console.log('To find vmalloc task_structs:');
console.log('- Walk ALL of PGD[256]\'s page tables');
console.log('- Find mapped regions that don\'t match linear formula');
console.log('- Check if they contain task_struct signatures');
console.log('- This would find the "clean" contiguous views!');
console.log('');
console.log('Or better: Follow kernel data structures (IDR)');
console.log('that already have the VA pointers!');

fs.closeSync(fd);