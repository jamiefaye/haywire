#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');
const fileSize = fs.statSync(filePath).size;

const GUEST_RAM_START = 0x40000000;

console.log('Searching for real kernel swapper_pg_dir with correct signature...');
console.log('Looking for: 1 user entry + 2-3 kernel entries\n');

// Search for PGDs
const candidates = [];
const searchStart = 0x0;  // Start from beginning of file
const searchEnd = Math.min(fileSize, 0x200000000 - GUEST_RAM_START); // Up to 8GB
const stride = 0x1000; // Page aligned

for (let offset = searchStart; offset < searchEnd; offset += stride) {
    const buffer = Buffer.allocUnsafe(4096);
    try {
        fs.readSync(fd, buffer, 0, 4096, offset);
    } catch (e) {
        continue;
    }
    
    let userEntries = 0;
    let kernelEntries = 0;
    let totalNonZero = 0;
    
    for (let i = 0; i < 512; i++) {
        const entry = buffer.readBigUint64LE(i * 8);
        if (entry !== 0n) {
            totalNonZero++;
            const type = entry & 0x3n;
            if (type === 0x3n || type === 0x1n) { // Valid table or block
                if (i < 256) {
                    userEntries++;
                } else {
                    kernelEntries++;
                }
            }
        }
    }
    
    // Look for the signature: 1 user entry, 2-3 kernel entries
    // Also check that entries look like valid page table entries
    if (userEntries === 1 && kernelEntries >= 2 && kernelEntries <= 3) {
        // Read first few entries to validate
        let looksValid = false;
        for (let i = 0; i < 512; i++) {
            const entry = buffer.readBigUint64LE(i * 8);
            if (entry !== 0n && (entry & 0x3n) >= 1) {
                // Check if it looks like a page table entry (has physical address in reasonable range)
                const pa_from_entry = Number(entry & 0x0000FFFFFFFFF000n);
                if (pa_from_entry > 0x1000 && pa_from_entry < 0x200000000) {
                    looksValid = true;
                    break;
                }
            }
        }

        if (looksValid) {
            const pa = offset + GUEST_RAM_START;
            candidates.push({
                offset,
                pa,
                userEntries,
                kernelEntries,
                totalNonZero
            });
        }
    }
}

console.log(`Found ${candidates.length} candidates with correct signature:\n`);

for (const c of candidates.slice(0, 10)) {
    console.log(`PA 0x${c.pa.toString(16)} (offset 0x${c.offset.toString(16)}):`);
    console.log(`  User entries: ${c.userEntries}, Kernel entries: ${c.kernelEntries}`);
    
    // Read the actual entries
    const buffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, buffer, 0, 4096, c.offset);
    
    console.log('  User space entries:');
    for (let i = 0; i < 256; i++) {
        const entry = buffer.readBigUint64LE(i * 8);
        if (entry !== 0n && (entry & 0x3n) >= 1) {
            console.log(`    [${i}]: 0x${entry.toString(16)}`);
        }
    }
    
    console.log('  Kernel space entries:');
    for (let i = 256; i < 512; i++) {
        const entry = buffer.readBigUint64LE(i * 8);
        if (entry !== 0n && (entry & 0x3n) >= 1) {
            console.log(`    [${i}]: 0x${entry.toString(16)}`);
        }
    }
    console.log('');
}

fs.closeSync(fd);