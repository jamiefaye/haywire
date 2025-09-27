#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');

// Test reading PUD tables directly at the physical addresses
const testCases = [
    { name: 'Xwayland', pudPA: 0x4239d000 },
    { name: 'update-notifier', pudPA: 0x1050ab000 },
];

console.log('Testing direct read at physical addresses (no offset adjustment):\n');

for (const test of testCases) {
    console.log(`Process: ${test.name}`);
    console.log(`  PUD PA: 0x${test.pudPA.toString(16)}`);

    // Try reading directly at the PA (treating it as file offset)
    if (test.pudPA + 64 <= fs.statSync(filePath).size) {
        const buffer = Buffer.allocUnsafe(64);
        fs.readSync(fd, buffer, 0, 64, test.pudPA);

        let nonZeroCount = 0;
        let firstNonZero = null;
        for (let i = 0; i < 8; i++) {
            const val = buffer.readBigUint64LE(i * 8);
            if (val !== 0n) {
                nonZeroCount++;
                if (!firstNonZero) {
                    firstNonZero = val;
                }
            }
        }

        if (nonZeroCount > 0) {
            console.log(`  ✓ Direct read has data (${nonZeroCount}/8 non-zero entries)`);
            console.log(`    First entry: 0x${firstNonZero.toString(16)}`);
        } else {
            console.log(`  ✗ Direct read is all zeros`);
        }
    }

    // Also try with GUEST_RAM_START subtracted
    const offsetWithRAM = test.pudPA - 0x40000000;
    if (offsetWithRAM >= 0 && offsetWithRAM + 64 <= fs.statSync(filePath).size) {
        console.log(`  With GUEST_RAM_START subtracted (offset 0x${offsetWithRAM.toString(16)}):`);

        const buffer = Buffer.allocUnsafe(64);
        fs.readSync(fd, buffer, 0, 64, offsetWithRAM);

        let nonZeroCount = 0;
        for (let i = 0; i < 8; i++) {
            const val = buffer.readBigUint64LE(i * 8);
            if (val !== 0n) nonZeroCount++;
        }

        if (nonZeroCount > 0) {
            console.log(`    ✓ Has data (${nonZeroCount}/8 non-zero)`);
        } else {
            console.log(`    ✗ All zeros`);
        }
    }

    console.log('');
}

// Let's also check if the swapper_pg_dir has better entries
console.log('Checking swapper_pg_dir at 0x136dbf000 (0xf6dbf000 in file):');
const swapperOffset = 0x136dbf000 - 0x40000000;

const swapperBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, swapperBuffer, 0, 4096, swapperOffset);

let nonZeroEntries = [];
for (let i = 0; i < 512; i++) {
    const entry = swapperBuffer.readBigUint64LE(i * 8);
    if (entry !== 0n) {
        const physAddr = entry & 0x0000FFFFFFFFF000n;
        nonZeroEntries.push({ idx: i, entry, physAddr });
        if (nonZeroEntries.length <= 5) {
            console.log(`  [${i}]: 0x${entry.toString(16)} -> PA: 0x${physAddr.toString(16)}`);
        }
    }
}
console.log(`  Total non-zero entries: ${nonZeroEntries.length}`);

fs.closeSync(fd);