#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');

// Test reading PGDs using simple translation
const testCases = [
    { name: 'Xwayland', pgdVA: 0xffff000002cfc000n },
    { name: 'update-notifier', pgdVA: 0xffff0000c09d1000n },
];

console.log('Testing if we can read PGDs after simple translation:\n');

for (const test of testCases) {
    console.log(`Process: ${test.name}`);
    console.log(`  PGD VA: 0x${test.pgdVA.toString(16)}`);

    // Simple translation - just mask out the 0xffff prefix
    const pgdPA = test.pgdVA & 0xFFFFFFFFFFFFn;
    console.log(`  PGD PA: 0x${pgdPA.toString(16)} (${(Number(pgdPA) / (1024*1024*1024)).toFixed(3)} GB)`);

    const offset = Number(pgdPA);

    if (offset + 4096 > fs.statSync(filePath).size) {
        console.log(`  ERROR: Offset 0x${offset.toString(16)} is beyond file size`);
        continue;
    }

    // Read first few PGD entries
    console.log(`  First 5 PGD entries:`);
    const buffer = Buffer.allocUnsafe(8 * 5);
    fs.readSync(fd, buffer, 0, 8 * 5, offset);

    let validEntries = 0;
    for (let i = 0; i < 5; i++) {
        const entry = buffer.readBigUint64LE(i * 8);
        if (entry !== 0n) {
            const typeBits = Number(entry & 3n);
            const physAddr = entry & 0x0000FFFFFFFFF000n;
            console.log(`    [${i}]: 0x${entry.toString(16)} -> PA: 0x${physAddr.toString(16)} (type: ${typeBits})`);
            validEntries++;
        }
    }

    if (validEntries === 0) {
        console.log(`    All entries are zero!`);
    } else {
        console.log(`    ✓ Found ${validEntries} non-zero entries`);
    }
    console.log('');
}

fs.closeSync(fd);