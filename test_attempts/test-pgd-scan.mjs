#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');

// Test reading PGDs using simple translation
const testCases = [
    { name: 'Xwayland', pgdVA: 0xffff000002cfc000n },
    { name: 'update-notifier', pgdVA: 0xffff0000c09d1000n },
];

console.log('Scanning PGD pages for non-zero entries:\n');

for (const test of testCases) {
    console.log(`Process: ${test.name}`);
    console.log(`  PGD VA: 0x${test.pgdVA.toString(16)}`);

    // Simple translation - just mask out the 0xffff prefix
    const pgdPA = test.pgdVA & 0xFFFFFFFFFFFFn;
    console.log(`  PGD PA: 0x${pgdPA.toString(16)}`);

    const offset = Number(pgdPA);

    if (offset + 4096 > fs.statSync(filePath).size) {
        console.log(`  ERROR: Offset 0x${offset.toString(16)} is beyond file size`);
        continue;
    }

    // Read entire 4KB PGD page
    const pageBuffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, pageBuffer, 0, 4096, offset);

    // Count non-zero entries
    let nonZeroCount = 0;
    let firstNonZero = -1;
    for (let i = 0; i < 512; i++) {  // 512 entries in a 4KB page
        const entry = pageBuffer.readBigUint64LE(i * 8);
        if (entry !== 0n) {
            nonZeroCount++;
            if (firstNonZero === -1) {
                firstNonZero = i;
                const typeBits = Number(entry & 3n);
                const physAddr = entry & 0x0000FFFFFFFFF000n;
                console.log(`  First non-zero at [${i}]: 0x${entry.toString(16)}`);
                console.log(`    Type: ${typeBits}, PA: 0x${physAddr.toString(16)}`);
            }
        }
    }

    if (nonZeroCount === 0) {
        console.log(`  ✗ Entire PGD page is zeros!`);

        // Maybe the PGD needs different translation? Check what's before/after
        console.log(`  Checking adjacent pages...`);

        // Check page before
        if (offset >= 4096) {
            const beforeBuffer = Buffer.allocUnsafe(64);
            fs.readSync(fd, beforeBuffer, 0, 64, offset - 64);
            let hasBefore = false;
            for (let i = 0; i < 8; i++) {
                const val = beforeBuffer.readBigUint64LE(i * 8);
                if (val !== 0n) hasBefore = true;
            }
            if (hasBefore) console.log(`    Page before has data`);
        }

        // Check page after
        if (offset + 4096 + 64 <= fs.statSync(filePath).size) {
            const afterBuffer = Buffer.allocUnsafe(64);
            fs.readSync(fd, afterBuffer, 0, 64, offset + 4096);
            let hasAfter = false;
            for (let i = 0; i < 8; i++) {
                const val = afterBuffer.readBigUint64LE(i * 8);
                if (val !== 0n) hasAfter = true;
            }
            if (hasAfter) console.log(`    Page after has data`);
        }
    } else {
        console.log(`  ✓ Found ${nonZeroCount} non-zero entries`);
    }
    console.log('');
}

fs.closeSync(fd);