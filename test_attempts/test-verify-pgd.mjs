#!/usr/bin/env node

import fs from 'fs';

const filePath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(filePath, 'r');
const GUEST_RAM_START = 0x40000000;

// Test translation like verifyKernelPgdByTranslation does
function testTranslation(pgdPA) {
    console.log(`Testing PGD at PA 0x${pgdPA.toString(16)}:`);
    
    // Read the PGD
    const pgdOffset = pgdPA - GUEST_RAM_START;
    const pgdBuffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, pgdBuffer, 0, 4096, pgdOffset);
    
    // Check for linear mapping - VA 0x1000 should map to PA 0x40001000
    // This would go through PGD[0] for user space
    const testVA = 0x1000;
    const pgdIdx = 0; // VA 0x1000 maps to PGD[0]
    
    const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
    console.log(`  PGD[${pgdIdx}] for VA 0x${testVA.toString(16)}: 0x${pgdEntry.toString(16)}`);
    
    if (pgdEntry && (pgdEntry & 0x3n) === 0x3n) {
        const pudPA = Number(pgdEntry & 0x0000FFFFFFFFF000n);
        console.log(`  -> Points to PUD at PA 0x${pudPA.toString(16)}`);
        
        // Would need to continue walking to verify, but this shows if entry exists
        return true;
    }
    
    // Also check if there's an entry at PGD[0] at all
    let hasUserEntry = false;
    for (let i = 0; i < 256; i++) {
        const entry = pgdBuffer.readBigUint64LE(i * 8);
        if (entry !== 0n && (entry & 0x3n) >= 1) {
            console.log(`  Has user entry at [${i}]: 0x${entry.toString(16)}`);
            hasUserEntry = true;
            break;
        }
    }
    
    // Check kernel entries
    let kernelEntries = 0;
    for (let i = 256; i < 512; i++) {
        const entry = pgdBuffer.readBigUint64LE(i * 8);
        if (entry !== 0n && (entry & 0x3n) >= 1) {
            kernelEntries++;
            if (kernelEntries <= 3) {
                console.log(`  Kernel entry [${i}]: 0x${entry.toString(16)}`);
            }
        }
    }
    console.log(`  Total kernel entries: ${kernelEntries}`);
    
    return hasUserEntry && kernelEntries >= 2 && kernelEntries <= 3;
}

// Test our candidates
const candidates = [
    0x4330c000,
    0x43f50000,
    0x4205a000  // The one currently being used
];

console.log('Testing PGD candidates for kernel swapper_pg_dir:\n');

for (const pgd of candidates) {
    const result = testTranslation(pgd);
    console.log(`  Result: ${result ? '✓ Could be kernel PGD' : '✗ Unlikely to be kernel PGD'}`);
    console.log('');
}

fs.closeSync(fd);