#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');

// Test reading from the simple translated addresses
const testAddresses = [
    { name: 'Xwayland', va: 0xffff0000c9fe8a00n, pa: 0xc9fe8a00n },
    { name: 'update-notifier', va: 0xffff0000027fee00n, pa: 0x027fee00n },
];

console.log('Testing if we can read mm_structs from simple translation:\n');

for (const test of testAddresses) {
    console.log(`Process: ${test.name}`);
    console.log(`  VA: 0x${test.va.toString(16)}`);
    console.log(`  Simple PA: 0x${test.pa.toString(16)}`);

    const offset = Number(test.pa);

    // Try to read PGD at offset 0x68
    const pgdOffset = offset + 0x68;

    if (pgdOffset + 8 > fs.statSync(filePath).size) {
        console.log(`  ERROR: Offset 0x${pgdOffset.toString(16)} is beyond file size`);
        continue;
    }

    const buffer = Buffer.allocUnsafe(8);
    fs.readSync(fd, buffer, 0, 8, pgdOffset);
    const pgd = buffer.readBigUint64LE();

    console.log(`  PGD at offset 0x68: 0x${pgd.toString(16)}`);

    // Check if it looks like a valid PGD
    if (pgd > 0x1000n && pgd < 0x200000000n) {
        console.log(`    ✓ Looks like a valid physical address!`);
    } else if (pgd === 0n) {
        console.log(`    ✗ PGD is NULL`);
    } else if (pgd > 0xffff000000000000n) {
        console.log(`    ⚠ PGD looks like a kernel VA, needs translation`);
    } else {
        console.log(`    ⚠ PGD value unclear`);
    }

    // Also read some context around offset 0x68
    console.log(`  Context around PGD offset:`);
    const contextBuffer = Buffer.allocUnsafe(32);
    fs.readSync(fd, contextBuffer, 0, 32, offset + 0x60);

    for (let i = 0; i < 4; i++) {
        const val = contextBuffer.readBigUint64LE(i * 8);
        console.log(`    [0x${(0x60 + i * 8).toString(16)}]: 0x${val.toString(16)}`);
    }

    console.log('');
}

fs.closeSync(fd);