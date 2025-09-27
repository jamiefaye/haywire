#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Deep walk of user-space mapping at PGD[0]\n');

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

// Let's examine PUD[0] in detail - it maps VAs 0x0 - 0x3FFFFFFF (1GB)
const pudEntry0 = pudBuffer.readBigUint64LE(0);
const pmdTablePA = Number(pudEntry0 & PA_MASK);

console.log(`PUD[0]: 0x${pudEntry0.toString(16)} -> PMD table at PA 0x${pmdTablePA.toString(16)}`);
console.log(`  Maps VA range: 0x00000000 - 0x3FFFFFFF (first 1GB of user space)\n`);

// Read PMD table
const pmdOffset = pmdTablePA - GUEST_RAM_START;
const pmdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pmdBuffer, 0, 4096, pmdOffset);

// Check first few PMD entries
console.log('PMD entries (each maps 2MB):');
for (let i = 0; i < 10; i++) {
    const pmdEntry = pmdBuffer.readBigUint64LE(i * 8);
    if (pmdEntry !== 0n) {
        const pmdType = pmdEntry & 0x3n;
        const pmdPA = Number(pmdEntry & PA_MASK);
        const vaStart = i * 0x200000; // Each PMD maps 2MB

        console.log(`  PMD[${i}]: 0x${pmdEntry.toString(16)}`);
        console.log(`    Type: ${pmdType === 0x3n ? 'table (4KB pages)' : pmdType === 0x1n ? 'block (2MB page)' : 'invalid'}`);
        console.log(`    PA: 0x${pmdPA.toString(16)}`);
        console.log(`    Maps VAs: 0x${vaStart.toString(16)} - 0x${(vaStart + 0x1FFFFF).toString(16)}`);

        // If it's a PTE table, look at some PTEs
        if (pmdType === 0x3n && i === 0) {
            console.log('    Walking down to PTE level...');
            const pteOffset = pmdPA - GUEST_RAM_START;
            const pteBuffer = Buffer.allocUnsafe(512); // Read first 64 PTEs
            fs.readSync(fd, pteBuffer, 0, 512, pteOffset);

            console.log('    First few PTEs (each maps 4KB):');
            for (let j = 0; j < 5; j++) {
                const pteEntry = pteBuffer.readBigUint64LE(j * 8);
                if (pteEntry !== 0n) {
                    const ptePA = Number(pteEntry & PA_MASK);
                    const pteVA = vaStart + j * 0x1000;
                    const flags = pteEntry & 0xFFFn;

                    // Decode some flags
                    const valid = (flags & 0x1n) ? 'V' : '-';
                    const af = (flags & 0x400n) ? 'A' : '-';  // Access flag
                    const ap2 = (flags & 0x80n) ? 'RO' : 'RW';  // Read-only or read-write
                    const ap1 = (flags & 0x40n) ? 'U' : 'K';   // User or kernel
                    const nx = (flags & 0x10000000000000n) ? 'NX' : 'X';  // No-execute

                    console.log(`      PTE[${j}]: VA 0x${pteVA.toString(16)} -> PA 0x${ptePA.toString(16)} [${valid}${af} ${ap1}:${ap2} ${nx}]`);
                }
            }
        }
    }
}

// Count total valid PMD entries
let totalPMDs = 0;
for (let i = 0; i < 512; i++) {
    const pmdEntry = pmdBuffer.readBigUint64LE(i * 8);
    if (pmdEntry !== 0n && (pmdEntry & 0x3n) >= 1) {
        totalPMDs++;
    }
}

console.log(`\nTotal valid PMD entries in PUD[0]: ${totalPMDs}/512`);
console.log(`This covers ${totalPMDs * 2}MB = ${totalPMDs * 2 / 1024}GB of mappings`);

// Now let's check what physical addresses these are pointing to
console.log('\n=== PHYSICAL ADDRESS ANALYSIS ===');
console.log('Checking what physical memory is being mapped...');

// Sample a few PTEs to see the physical addresses
const pmdEntry0 = pmdBuffer.readBigUint64LE(0);
if ((pmdEntry0 & 0x3n) === 0x3n) {
    const pteTablePA = Number(pmdEntry0 & PA_MASK);
    const pteOffset = pteTablePA - GUEST_RAM_START;
    const pteSample = Buffer.allocUnsafe(256);
    fs.readSync(fd, pteSample, 0, 256, pteOffset);

    const physAddrs = [];
    for (let i = 0; i < 32; i++) {
        const pte = pteSample.readBigUint64LE(i * 8);
        if (pte !== 0n) {
            physAddrs.push(Number(pte & PA_MASK));
        }
    }

    if (physAddrs.length > 0) {
        console.log(`Sample of physical addresses being mapped:`);
        console.log(`  First PA: 0x${physAddrs[0].toString(16)}`);
        console.log(`  Last PA: 0x${physAddrs[physAddrs.length-1].toString(16)}`);

        // Check if they're sequential
        let sequential = true;
        for (let i = 1; i < physAddrs.length; i++) {
            if (physAddrs[i] !== physAddrs[i-1] + 0x1000) {
                sequential = false;
                break;
            }
        }

        if (sequential) {
            console.log(`  ✓ Physical addresses are SEQUENTIAL (identity/linear mapping)`);
            console.log(`  This appears to be mapping physical RAM directly into user space`);
        } else {
            console.log(`  Physical addresses are NOT sequential`);
        }
    }
}

fs.closeSync(fd);

console.log('\n=== CONCLUSION ===');
console.log('This extensive user-space mapping in the kernel PGD appears to be:');
console.log('- A direct/linear mapping of physical memory');
console.log('- Possibly for kernel access to user memory without translation');
console.log('- Or for early boot before proper user PGDs are set up');
console.log('- Could also be related to how ARM64 handles certain operations');