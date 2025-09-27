#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');
const fileSize = fs.statSync(filePath).size;

// Test the PGD entries we found
const testCases = [
    { name: 'Xwayland', pgdPA: 0x2cfc000, entryIdx: 389, entry: 0x80000004239d403n },
    { name: 'update-notifier', pgdPA: 0xc09d1000, entryIdx: 372, entry: 0x8000001050ab403n },
];

console.log('Analyzing PGD entries:\n');
console.log(`File size: ${(fileSize / (1024*1024*1024)).toFixed(2)} GB\n`);

for (const test of testCases) {
    console.log(`Process: ${test.name}`);
    console.log(`  PGD entry [${test.entryIdx}]: 0x${test.entry.toString(16)}`);

    // Extract physical address from entry
    const physAddr = test.entry & 0x0000FFFFFFFFF000n;
    const typeBits = Number(test.entry & 3n);

    console.log(`  Physical address: 0x${physAddr.toString(16)} (${(Number(physAddr) / (1024*1024*1024)).toFixed(3)} GB)`);
    console.log(`  Type bits: ${typeBits} (${typeBits === 3 ? 'Table descriptor' : 'Invalid/Block'})`);

    // Check if physical address is within file bounds
    if (Number(physAddr) >= fileSize) {
        console.log(`  ✗ Physical address is OUTSIDE file bounds (${(fileSize / (1024*1024*1024)).toFixed(2)} GB)`);
    } else if (Number(physAddr) < 0x40000000) {
        console.log(`  ⚠ Physical address is below GUEST_RAM_START (0x40000000)`);

        // Maybe we need to add GUEST_RAM_START?
        const adjustedPA = Number(physAddr) + 0x40000000;
        console.log(`  Adjusted PA: 0x${adjustedPA.toString(16)} (${(adjustedPA / (1024*1024*1024)).toFixed(3)} GB)`);

        if (adjustedPA < fileSize) {
            // Try reading from adjusted address
            const buffer = Buffer.allocUnsafe(64);
            fs.readSync(fd, buffer, 0, 64, adjustedPA - 0x40000000);

            let nonZeroCount = 0;
            for (let i = 0; i < 8; i++) {
                const val = buffer.readBigUint64LE(i * 8);
                if (val !== 0n) nonZeroCount++;
            }

            if (nonZeroCount > 0) {
                console.log(`  ✓ Adjusted PA has data (${nonZeroCount}/8 non-zero entries)`);
            } else {
                console.log(`  ✗ Adjusted PA is all zeros`);
            }
        }
    } else {
        console.log(`  ✓ Physical address is within file bounds`);

        // Try reading from this address
        const offset = Number(physAddr) - 0x40000000;
        if (offset >= 0 && offset + 64 <= fileSize) {
            const buffer = Buffer.allocUnsafe(64);
            fs.readSync(fd, buffer, 0, 64, offset);

            let nonZeroCount = 0;
            for (let i = 0; i < 8; i++) {
                const val = buffer.readBigUint64LE(i * 8);
                if (val !== 0n) nonZeroCount++;
            }

            if (nonZeroCount > 0) {
                console.log(`  ✓ Target page has data (${nonZeroCount}/8 non-zero entries)`);
                // Show first entry
                const firstEntry = buffer.readBigUint64LE(0);
                console.log(`    First PUD entry: 0x${firstEntry.toString(16)}`);
            } else {
                console.log(`  ✗ Target page is all zeros`);
            }
        } else {
            console.log(`  ✗ Cannot read from offset ${offset} (negative or beyond file)`);
        }
    }

    console.log('');
}

// Also check what PGD index these correspond to
console.log('Virtual address analysis:');
for (const test of testCases) {
    // PGD index 389 corresponds to VA bits [47:39]
    // VA = pgd_index << 39
    const vaBase = BigInt(test.entryIdx) << 39n;
    console.log(`  PGD[${test.entryIdx}] maps VA range: 0x${vaBase.toString(16)} - 0x${((vaBase + (1n << 39n)) - 1n).toString(16)}`);
}

fs.closeSync(fd);