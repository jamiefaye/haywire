#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Complete Analysis of PGD[256] Mappings ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Read swapper_pg_dir
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);

const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
if (pgd256 === 0n) {
    console.log('PGD[256] is empty!');
    process.exit(1);
}

const pudTablePA = Number(pgd256 & PA_MASK & ~0xFFFn);
console.log(`PGD[256] -> PUD table at PA 0x${pudTablePA.toString(16)}`);
console.log('');

// Read PUD table
const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);

console.log('PUD Entries in PGD[256]:\n');
console.log('Index | VA Start           | Entry              | Type');
console.log('------+--------------------+--------------------+--------------');

for (let i = 0; i < 512; i++) {
    const pudEntry = pudBuffer.readBigUint64LE(i * 8);
    if (pudEntry === 0n) continue;
    
    const vaStart = 0xFFFF800000000000n + (BigInt(i) << 30n);
    let type = 'Unknown';
    
    if ((pudEntry & 0x3n) === 0x1n) {
        type = '1GB block';
    } else if ((pudEntry & 0x3n) === 0x3n) {
        type = 'Table';
    }
    
    console.log(`${i.toString().padStart(5)} | 0x${vaStart.toString(16)} | 0x${pudEntry.toString(16).padStart(16, '0')} | ${type}`);
}

console.log('');
console.log('Summary:');
console.log('--------');

// Analyze what we have
let blockCount = 0;
let tableCount = 0;
let linearMapStart = -1;
let linearMapEnd = -1;

for (let i = 0; i < 512; i++) {
    const pudEntry = pudBuffer.readBigUint64LE(i * 8);
    if (pudEntry === 0n) continue;
    
    if ((pudEntry & 0x3n) === 0x1n) {
        blockCount++;
        if (linearMapStart === -1) linearMapStart = i;
        linearMapEnd = i;
    } else if ((pudEntry & 0x3n) === 0x3n) {
        tableCount++;
    }
}

if (linearMapStart !== -1) {
    const startVA = 0xFFFF800000000000n + (BigInt(linearMapStart) << 30n);
    const endVA = 0xFFFF800000000000n + (BigInt(linearMapEnd + 1) << 30n);
    const sizeGB = Number((endVA - startVA) / 0x40000000n);
    console.log(`Linear map: PUD[${linearMapStart}] - PUD[${linearMapEnd}]`);
    console.log(`  VA range: 0x${startVA.toString(16)} - 0x${endVA.toString(16)}`);
    console.log(`  Size: ${sizeGB} GB (${blockCount} x 1GB blocks)`);
    console.log(`  This maps our 6GB of RAM linearly`);
}

if (tableCount > 0) {
    console.log(`\nTable mappings: ${tableCount} PUD entries point to PMD tables`);
    console.log(`  These contain vmalloc and other fine-grained mappings`);
}

console.log('');
console.log('Key Insight:');
console.log('-----------');
console.log('To find 100% of processes, we need to scan:');
console.log('1. The linear map (1GB blocks) - contains 91% of processes');
console.log('2. The table mappings - contain the missing 9% in vmalloc');
console.log('');
console.log('Our current scanner only looks at table mappings (small pages).');
console.log('We need to ALSO scan the 1GB block mappings!');

fs.closeSync(fd);