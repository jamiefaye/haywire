#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Investigating PGD index 0 in kernel swapper_pg_dir\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the known kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Get entry at index 0
const entry0 = pgdBuffer.readBigUint64LE(0);
console.log(`PGD[0]: 0x${entry0.toString(16)}`);

// Decode the entry
const type = entry0 & 0x3n;
const physAddr = Number(entry0 & PA_MASK);

console.log(`  Type: ${type === 0x3n ? 'table' : type === 0x1n ? 'block' : 'invalid'}`);
console.log(`  Physical address: 0x${physAddr.toString(16)}`);

// Calculate what virtual address range this maps
// PGD index 0 in user space maps VAs 0x0000000000000000 - 0x0000008000000000 (512GB)
console.log(`  Maps VAs: 0x0000000000000000 - 0x0000007FFFFFFFFF (512GB at start of user space)`);

// This is VERY unusual for a kernel PGD!
console.log('\n⚠️  This is UNUSUAL for swapper_pg_dir!');
console.log('The kernel PGD typically has NO user-space mappings.');
console.log('User processes have their own PGDs with user mappings.');
console.log('\nPossible explanations:');
console.log('1. This might be for kernel threads that need minimal user mappings');
console.log('2. Could be for the idle task (PID 0) which runs when nothing else is scheduled');
console.log('3. Might be identity mapping for early boot or special purposes');

// Let's walk down and see what's actually mapped
if (type === 0x3n) {
    console.log('\nWalking down to see what\'s mapped...');

    const pudTableOffset = physAddr - GUEST_RAM_START;
    const pudBuffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, pudBuffer, 0, 4096, pudTableOffset);

    // Check PUD entries
    console.log('PUD entries (second level):');
    for (let i = 0; i < 512; i++) {
        const pudEntry = pudBuffer.readBigUint64LE(i * 8);
        if (pudEntry !== 0n && (pudEntry & 0x3n) >= 1) {
            const pudPA = Number(pudEntry & PA_MASK);
            const pudType = pudEntry & 0x3n;
            console.log(`  PUD[${i}]: 0x${pudEntry.toString(16)}`);
            console.log(`    Type: ${pudType === 0x3n ? 'table' : 'block'}, PA: 0x${pudPA.toString(16)}`);

            // Calculate VA range for this PUD entry
            const vaStart = BigInt(i) * (1n << 30n); // Each PUD entry covers 1GB
            const vaEnd = vaStart + (1n << 30n) - 1n;
            console.log(`    Maps VAs: 0x${vaStart.toString(16)} - 0x${vaEnd.toString(16)}`);

            // If it's a table, check PMD level
            if (pudType === 0x3n && i < 5) { // Only check first few
                const pmdTableOffset = pudPA - GUEST_RAM_START;
                const pmdBuffer = Buffer.allocUnsafe(512); // Just check first 64 entries
                fs.readSync(fd, pmdBuffer, 0, 512, pmdTableOffset);

                let pmdValidCount = 0;
                for (let j = 0; j < 64; j++) {
                    const pmdEntry = pmdBuffer.readBigUint64LE(j * 8);
                    if (pmdEntry !== 0n && (pmdEntry & 0x3n) >= 1) {
                        pmdValidCount++;
                        if (pmdValidCount === 1) {
                            console.log(`    First PMD entry: PMD[${j}] = 0x${pmdEntry.toString(16)}`);
                            const pmdVA = vaStart + BigInt(j) * (1n << 21n); // Each PMD is 2MB
                            console.log(`      Maps VA: 0x${pmdVA.toString(16)}`);
                        }
                    }
                }
                console.log(`    PMD level has ${pmdValidCount} valid entries (checked first 64)`);
            }
        }
    }
}

fs.closeSync(fd);

console.log('\n=== ANALYSIS ===');
console.log('This user-space mapping in the kernel PGD is likely for:');
console.log('- VDSO (Virtual Dynamic Shared Object) - kernel code callable from userspace');
console.log('- Signal trampolines');
console.log('- Other kernel-provided user mappings that all processes share');
console.log('These need to be in swapper_pg_dir so they\'re available to all processes.');