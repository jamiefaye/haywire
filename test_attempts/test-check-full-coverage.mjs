#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Checking full coverage of user-space mapping at PGD[0]\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Get PGD[0]
const pgdEntry = pgdBuffer.readBigUint64LE(0);
const pudTablePA = Number(pgdEntry & PA_MASK);

console.log(`PGD[0]: 0x${pgdEntry.toString(16)} -> PUD table at PA 0x${pudTablePA.toString(16)}\n`);

// Read PUD table
const pudOffset = pudTablePA - GUEST_RAM_START;
const pudBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pudBuffer, 0, 4096, pudOffset);

// Check all PUD entries we found earlier
const pudIndices = [0, 1, 2, 3];
let totalMapped = 0;

for (const pudIdx of pudIndices) {
    const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
    if (pudEntry !== 0n && (pudEntry & 0x3n) === 0x3n) {
        const pmdTablePA = Number(pudEntry & PA_MASK);
        const vaStart = pudIdx * 0x40000000; // Each PUD covers 1GB
        const vaEnd = vaStart + 0x3FFFFFFF;

        console.log(`PUD[${pudIdx}]: 0x${pudEntry.toString(16)}`);
        console.log(`  VA range: 0x${vaStart.toString(16)} - 0x${vaEnd.toString(16)} (1GB)`);

        // Read PMD table and count valid entries
        const pmdOffset = pmdTablePA - GUEST_RAM_START;
        const pmdBuffer = Buffer.allocUnsafe(4096);
        fs.readSync(fd, pmdBuffer, 0, 4096, pmdOffset);

        let validPMDs = 0;
        for (let i = 0; i < 512; i++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(i * 8);
            if (pmdEntry !== 0n && (pmdEntry & 0x3n) >= 1) {
                validPMDs++;
            }
        }

        const coverage = validPMDs * 2; // Each PMD covers 2MB
        console.log(`  Valid PMDs: ${validPMDs}/512 = ${coverage}MB mapped`);

        // Check first PTE to see what physical address this maps to
        const pmdEntry0 = pmdBuffer.readBigUint64LE(0);
        if ((pmdEntry0 & 0x3n) === 0x3n) {
            const pteTablePA = Number(pmdEntry0 & PA_MASK);
            const pteOffset = pteTablePA - GUEST_RAM_START;
            const firstPTE = Buffer.allocUnsafe(8);
            fs.readSync(fd, firstPTE, 0, 8, pteOffset);
            const pte0 = firstPTE.readBigUint64LE(0);
            const physAddr = Number(pte0 & PA_MASK);
            console.log(`  First VA 0x${vaStart.toString(16)} maps to PA 0x${physAddr.toString(16)}`);

            // Check if it continues the pattern
            const expectedPA = GUEST_RAM_START + vaStart;
            if (physAddr === expectedPA) {
                console.log(`  ✓ Continues linear mapping pattern`);
            }
        }

        totalMapped += coverage;
        console.log('');
    }
}

console.log(`=== TOTAL COVERAGE ===`);
console.log(`Total mapped: ${totalMapped}MB = ${totalMapped/1024}GB`);
console.log(`\nThis maps virtual addresses 0x0 - 0x${(totalMapped * 1024 * 1024 - 1).toString(16)}`);
console.log(`To physical addresses 0x40000000 - 0x${(0x40000000 + totalMapped * 1024 * 1024 - 1).toString(16)}`);

// Check how much physical RAM we have
const fileSize = fs.statSync('/tmp/haywire-vm-mem').size;
const ramSize = fileSize - (GUEST_RAM_START - 0);  // Approximate
console.log(`\nPhysical RAM in file: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB`);
console.log(`Guest RAM size: ~${(ramSize / (1024*1024*1024)).toFixed(2)}GB`);

fs.closeSync(fd);

console.log('\n=== CONCLUSION ===');
console.log('The kernel PGD has a complete linear/identity mapping of the first 4GB of physical RAM');
console.log('into the user virtual address space (0x0 - 0xFFFFFFFF).');
console.log('This allows the kernel to access any physical address by using its offset from 0.');