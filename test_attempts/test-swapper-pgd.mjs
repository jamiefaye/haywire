#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');
const fileSize = fs.statSync(filePath).size;

const GUEST_RAM_START = 0x40000000;
const swapperPgd = 0x4205a000;  // Current discovered address from jc.log

console.log(`Testing swapper_pg_dir at PA 0x${swapperPgd.toString(16)}\n`);

// Read the swapper_pg_dir page table
const pgdOffset = swapperPgd - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdOffset);

let validKernelEntries = 0;
let validUserEntries = 0;
let firstKernelEntry = null;
let firstUserEntry = null;

// Check all 512 entries
for (let i = 0; i < 512; i++) {
    const entry = pgdBuffer.readBigUint64LE(i * 8);
    if (entry !== 0n) {
        const type = entry & 0x3n;
        if (type === 0x3n) { // Valid table descriptor
            if (i >= 256) {
                validKernelEntries++;
                if (!firstKernelEntry) {
                    firstKernelEntry = { idx: i, entry };
                }
            } else {
                validUserEntries++;
                if (!firstUserEntry) {
                    firstUserEntry = { idx: i, entry };
                }
            }
        }
    }
}

console.log(`User space entries (0-255): ${validUserEntries} valid`);
if (firstUserEntry) {
    console.log(`  First at [${firstUserEntry.idx}]: 0x${firstUserEntry.entry.toString(16)}`);
}

console.log(`\nKernel space entries (256-511): ${validKernelEntries} valid`);
if (firstKernelEntry) {
    console.log(`  First at [${firstKernelEntry.idx}]: 0x${firstKernelEntry.entry.toString(16)}`);
    
    // Try to read the first kernel PUD
    const pudPA = Number(firstKernelEntry.entry & 0x0000FFFFFFFFF000n);
    console.log(`\n  Checking PUD at PA 0x${pudPA.toString(16)}`);

    // PUD PA should be used directly as file offset (no GUEST_RAM_START adjustment)
    if (pudPA < fileSize) {
        const pudBuffer = Buffer.allocUnsafe(4096);
        fs.readSync(fd, pudBuffer, 0, 4096, pudPA);
        
        let pudValidCount = 0;
        let totalNonZero = 0;
        for (let j = 0; j < 512; j++) {  // Check all 512 PUD entries
            const pudEntry = pudBuffer.readBigUint64LE(j * 8);
            if (pudEntry !== 0n) {
                totalNonZero++;
                if ((pudEntry & 0x3n) === 0x3n || (pudEntry & 0x3n) === 0x1n) {
                    pudValidCount++;
                    if (pudValidCount <= 3) {
                        const type = (pudEntry & 0x3n) === 0x3n ? 'table' : 'block';
                        console.log(`    PUD[${j}]: 0x${pudEntry.toString(16)} (${type})`);
                    }
                }
            }
        }
        console.log(`    Found ${pudValidCount} valid PUD entries (${totalNonZero} non-zero)`);
    } else {
        console.log(`    PUD PA 0x${pudPA.toString(16)} is outside file range`);
    }
}

// Show raw hex dump of interesting area
console.log(`\nRaw hex dump of PGD entries 256-263:`);
for (let i = 256; i < 264; i++) {
    const entry = pgdBuffer.readBigUint64LE(i * 8);
    console.log(`  [${i}]: 0x${entry.toString(16).padStart(16, '0')}`);
}

fs.closeSync(fd);