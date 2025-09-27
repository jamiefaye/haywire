#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Testing PGD walk with offset calculation fix\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the known kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Find the kernel entries (we know they're at 256, 507, 511)
const kernelIndices = [256, 507, 511];

for (const idx of kernelIndices) {
    const entry = pgdBuffer.readBigUint64LE(idx * 8);
    if (entry !== 0n && (entry & 0x3n) === 0x3n) {  // Table descriptor
        const pudTablePA = Number(entry & PA_MASK);
        console.log(`\nPGD[${idx}]: 0x${entry.toString(16)}`);
        console.log(`  PUD table PA: 0x${pudTablePA.toString(16)}`);

        // Test both offset calculation methods
        console.log('\n  Method 1 (BROKEN - conditional):');
        const offsetMethod1 = pudTablePA >= GUEST_RAM_START
            ? pudTablePA - GUEST_RAM_START
            : pudTablePA;
        console.log(`    File offset: 0x${offsetMethod1.toString(16)}`);

        // Try to read with method 1
        try {
            const testBuf1 = Buffer.allocUnsafe(64);
            fs.readSync(fd, testBuf1, 0, 64, offsetMethod1);
            let validCount1 = 0;
            for (let i = 0; i < 8; i++) {
                const e = testBuf1.readBigUint64LE(i * 8);
                if (e !== 0n && (e & 0x3n) >= 1) validCount1++;
            }
            console.log(`    Valid entries in first 8: ${validCount1}`);
        } catch (err) {
            console.log(`    ERROR: ${err.message}`);
        }

        console.log('\n  Method 2 (FIXED - always subtract):');
        const offsetMethod2 = pudTablePA - GUEST_RAM_START;
        console.log(`    File offset: 0x${offsetMethod2.toString(16)}`);

        // Try to read with method 2
        try {
            const testBuf2 = Buffer.allocUnsafe(64);
            fs.readSync(fd, testBuf2, 0, 64, offsetMethod2);
            let validCount2 = 0;
            for (let i = 0; i < 8; i++) {
                const e = testBuf2.readBigUint64LE(i * 8);
                if (e !== 0n && (e & 0x3n) >= 1) validCount2++;
            }
            console.log(`    Valid entries in first 8: ${validCount2}`);

            // Show first valid entry if any
            for (let i = 0; i < 8; i++) {
                const e = testBuf2.readBigUint64LE(i * 8);
                if (e !== 0n && (e & 0x3n) >= 1) {
                    console.log(`    First valid entry: PUD[${i}] = 0x${e.toString(16)}`);
                    break;
                }
            }
        } catch (err) {
            console.log(`    ERROR: ${err.message}`);
        }
    }
}

fs.closeSync(fd);

console.log('\n=== CONCLUSION ===');
console.log('The issue is that PUD/PMD/PTE table addresses are already physical addresses');
console.log('in the guest address space (e.g., 0x138199000), which is ABOVE GUEST_RAM_START.');
console.log('The conditional check causes the code to NOT subtract GUEST_RAM_START,');
console.log('resulting in invalid file offsets. The fix is to ALWAYS subtract GUEST_RAM_START.');