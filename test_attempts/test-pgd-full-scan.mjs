#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Full scan of kernel PGD at 0x136DEB000\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the known kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Scan ALL 512 entries
const validEntries = [];
let userCount = 0;
let kernelCount = 0;

for (let idx = 0; idx < 512; idx++) {
    const entry = pgdBuffer.readBigUint64LE(idx * 8);
    if (entry !== 0n) {
        const type = entry & 0x3n;
        if (type === 0x1n || type === 0x3n) {  // Valid descriptor
            const physAddr = Number(entry & PA_MASK);
            validEntries.push({
                index: idx,
                entry: entry,
                type: type === 0x3n ? 'table' : 'block',
                physAddr: physAddr,
                isKernel: idx >= 256
            });

            if (idx < 256) userCount++;
            else kernelCount++;
        }
    }
}

console.log(`Found ${validEntries.length} valid entries:`);
console.log(`  User space (0-255): ${userCount}`);
console.log(`  Kernel space (256-511): ${kernelCount}`);
console.log('\nAll valid entries:');

for (const e of validEntries) {
    const space = e.isKernel ? 'KERNEL' : 'USER  ';
    console.log(`\n[${e.index.toString().padStart(3)}] ${space}: 0x${e.entry.toString(16)}`);
    console.log(`      Type: ${e.type}, PA: 0x${e.physAddr.toString(16)}`);

    // If it's a table descriptor, check what's in the next level
    if (e.type === 'table') {
        const nextTableOffset = e.physAddr - GUEST_RAM_START;
        const nextTableBuf = Buffer.allocUnsafe(4096);

        try {
            fs.readSync(fd, nextTableBuf, 0, 4096, nextTableOffset);

            // Count valid entries in the next level
            let validInNext = 0;
            const validIndices = [];
            for (let i = 0; i < 512; i++) {
                const nextEntry = nextTableBuf.readBigUint64LE(i * 8);
                if (nextEntry !== 0n && (nextEntry & 0x3n) >= 1) {
                    validInNext++;
                    if (validIndices.length < 10) {
                        validIndices.push(i);
                    }
                }
            }

            console.log(`      Next level has ${validInNext} valid entries`);
            if (validIndices.length > 0) {
                console.log(`      Valid at indices: ${validIndices.join(', ')}${validInNext > 10 ? '...' : ''}`);

                // Show first valid entry
                const firstIdx = validIndices[0];
                const firstEntry = nextTableBuf.readBigUint64LE(firstIdx * 8);
                console.log(`      First entry: [${firstIdx}] = 0x${firstEntry.toString(16)}`);
            }
        } catch (err) {
            console.log(`      ERROR reading next level: ${err.message}`);
        }
    }
}

fs.closeSync(fd);

console.log('\n=== EXPECTED vs ACTUAL ===');
console.log('We found entries at indices:', validEntries.map(e => e.index).join(', '));
console.log('You mentioned expecting entries around 256 and 502 (or similar high indices)');
if (!validEntries.find(e => e.index === 502)) {
    console.log('NOTE: No entry at index 502, but we have 507 and 511')
}