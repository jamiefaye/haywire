#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== What is ACTUALLY in PGD[0]? ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Read swapper_pg_dir
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);

console.log('PGD[0] entry: 0x' + pgdBuffer.readBigUint64LE(0).toString(16));
console.log('This maps VA range: 0x0 - 0x7FFFFFFFFF\n');

// Read PUD table that PGD[0] points to
const pgd0Entry = pgdBuffer.readBigUint64LE(0);
const pud0TablePA = Number(pgd0Entry & PA_MASK & ~0xFFFn);
console.log(`PGD[0] points to PUD table at PA: 0x${pud0TablePA.toString(16)}\n`);

const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pud0TablePA - GUEST_RAM_START);

// Check ALL PUD entries to see what's mapped
console.log('Scanning all 512 PUD entries in PGD[0]\'s table...\n');

const mappedPuds = [];
for (let i = 0; i < 512; i++) {
    const pudEntry = pudBuffer.readBigUint64LE(i * 8);
    if (pudEntry !== 0n) {
        const vaStart = BigInt(i) << 30n;  // Each PUD covers 1GB
        mappedPuds.push({ index: i, entry: pudEntry, vaStart });
    }
}

console.log(`Found ${mappedPuds.length} mapped PUD entries:\n`);

for (const pud of mappedPuds) {
    const vaStart = pud.vaStart;
    const vaEnd = vaStart + 0x40000000n - 1n;  // 1GB range
    console.log(`PUD[${pud.index}]: VA 0x${vaStart.toString(16)} - 0x${vaEnd.toString(16)}`);
    console.log(`  Entry: 0x${pud.entry.toString(16)}`);
    
    // Check if it's a block or table
    if ((pud.entry & 0x3n) === 0x1n) {
        // 1GB block
        const blockPA = Number(pud.entry & PA_MASK & ~0x3FFFFFFFn);
        console.log(`  Type: 1GB BLOCK`);
        console.log(`  Maps to PA: 0x${blockPA.toString(16)} - 0x${(blockPA + 0x3FFFFFFF).toString(16)}`);
        
        // Check if it's identity/linear
        const expectedPA = Number(vaStart) + GUEST_RAM_START;
        if (blockPA === expectedPA) {
            console.log(`  ✓ LINEAR MAPPING (VA + 0x${GUEST_RAM_START.toString(16)} = PA)`);
        } else {
            console.log(`  Offset: PA = VA + 0x${(blockPA - Number(vaStart)).toString(16)}`);
        }
    } else {
        // Page table
        const tablePA = Number(pud.entry & PA_MASK & ~0xFFFn);
        console.log(`  Type: Page table at PA 0x${tablePA.toString(16)}`);
        
        // Sample a few PMD entries to understand the mapping
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, tablePA - GUEST_RAM_START);
        
        let pmdMapped = 0;
        let firstPA = null;
        for (let j = 0; j < 512; j++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(j * 8);
            if (pmdEntry !== 0n) {
                pmdMapped++;
                if (firstPA === null && (pmdEntry & 0x3n) === 0x1n) {
                    firstPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                }
            }
        }
        console.log(`  Has ${pmdMapped}/512 PMD entries mapped`);
        if (firstPA !== null) {
            console.log(`  First block maps to PA 0x${firstPA.toString(16)}`);
        }
    }
    console.log('');
}

console.log('=== Analysis ===\n');

if (mappedPuds.length === 0) {
    console.log('PGD[0] has NO mappings! It\'s empty in swapper context.');
    console.log('This makes sense - swapper (kernel) doesn\'t need user-space mappings.');
} else if (mappedPuds.length <= 4) {
    console.log(`PGD[0] has ${mappedPuds.length} PUD entries (${mappedPuds.length} GB of VA space)`);
    console.log('This appears to be a LIMITED mapping, possibly:');
    console.log('- Direct physical memory access for kernel');
    console.log('- Special kernel mappings');
    console.log('- NOT full user-space (which would need more)');
} else {
    console.log(`PGD[0] has ${mappedPuds.length} PUD entries`);
    console.log('This is substantial mapping in the low VA range.');
}

console.log('');

// Now check where kernel typically lives
console.log('=== Where does the kernel actually live? ===\n');
console.log('ARM64 Linux kernel virtual memory layout:');
console.log('- 0xffff000000000000+: Kernel space starts here');
console.log('- 0xffff800000000000+: Linear mapping (direct map) typically here');
console.log('- PGD[256] and above handle these high addresses');
console.log('');
console.log('PGD[0] (VA 0x0 - 0x7FFFFFFFFF) is USER SPACE range.');
console.log('In swapper context, it might be:');
console.log('1. Empty (no user mappings needed)');
console.log('2. Special direct mappings for kernel convenience');
console.log('3. Temporary mappings');
console.log('');
console.log('The init_task VA 0xffff800083739840 is in PGD[256], NOT PGD[0]!');

fs.closeSync(fd);