#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;  // The known swapper PGD location
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Examining known swapper PGD at 0x136DEB000...\n');

// Open memory file
const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const fileSize = fs.statSync('/tmp/haywire-vm-mem').size;

// Calculate file offset
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;

console.log(`PGD Physical Address: 0x${KNOWN_SWAPPER_PGD.toString(16)}`);
console.log(`File offset: 0x${pgdFileOffset.toString(16)}`);
console.log(`File size: ${(fileSize / (1024*1024*1024)).toFixed(2)} GB\n`);

// Check if offset is valid
if (pgdFileOffset < 0 || pgdFileOffset + 4096 > fileSize) {
    console.log('ERROR: PGD offset is outside file bounds!');
    console.log(`  File size: 0x${fileSize.toString(16)}`);
    console.log(`  Need offset: 0x${pgdFileOffset.toString(16)}`);
    fs.closeSync(fd);
    process.exit(1);
}

// Read the PGD page
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Scan all 512 entries
let totalNonZero = 0;
let userEntries = 0;  // Entries 0-255
let kernelEntries = 0; // Entries 256-511
const validEntries = [];

console.log('Scanning all 512 PGD entries...\n');

for (let i = 0; i < 512; i++) {
    const entry = pgdBuffer.readBigUint64LE(i * 8);

    if (entry !== 0n) {
        totalNonZero++;
        const type = entry & 0x3n;

        // Check if it's a valid descriptor (type 1 or 3)
        if (type === 0x1n || type === 0x3n) {
            const physAddr = Number(entry & PA_MASK);
            const entryInfo = {
                index: i,
                entry: entry,
                type: type === 0x3n ? 'table' : 'block',
                physAddr: physAddr,
                isUser: i < 256
            };

            validEntries.push(entryInfo);

            if (i < 256) {
                userEntries++;
            } else {
                kernelEntries++;
            }

            // Print first few entries
            if (validEntries.length <= 10) {
                const space = i < 256 ? 'USER  ' : 'KERNEL';
                console.log(`[${i.toString().padStart(3)}] ${space}: 0x${entry.toString(16).padStart(16, '0')} (${entryInfo.type}) -> PA 0x${physAddr.toString(16)}`);
            }
        }
    }
}

console.log('\n=== SUMMARY ===');
console.log(`Total non-zero entries: ${totalNonZero}`);
console.log(`Valid entries (type 1 or 3): ${validEntries.length}`);
console.log(`  User space (0-255): ${userEntries}`);
console.log(`  Kernel space (256-511): ${kernelEntries}`);

if (validEntries.length === 0) {
    console.log('\nWARNING: No valid entries found!');
    console.log('Checking if page is all zeros...');

    let allZero = true;
    for (let i = 0; i < 4096; i++) {
        if (pgdBuffer[i] !== 0) {
            allZero = false;
            break;
        }
    }

    if (allZero) {
        console.log('ERROR: Page is all zeros!');
    } else {
        console.log('Page has data but no valid PGD entries.');
        console.log('\nFirst 256 bytes of page (hex dump):');
        for (let i = 0; i < 256; i += 16) {
            const line = pgdBuffer.slice(i, i + 16);
            const hex = Array.from(line).map(b => b.toString(16).padStart(2, '0')).join(' ');
            const ascii = Array.from(line).map(b => (b >= 32 && b <= 126) ? String.fromCharCode(b) : '.').join('');
            console.log(`  ${i.toString(16).padStart(4, '0')}: ${hex}  |${ascii}|`);
        }
    }
} else {
    console.log('\n✓ Found the expected entries at the correct location!');
}

fs.closeSync(fd);