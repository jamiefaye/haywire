#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== PGD[256]: The REAL Kernel Space ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Read swapper_pg_dir
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);

// PGD[256] maps VA 0xffff800000000000 - 0xffff807FFFFFFFFF
const pgd256Entry = pgdBuffer.readBigUint64LE(256 * 8);
console.log('PGD[256] entry: 0x' + pgd256Entry.toString(16));
console.log('Maps VA range: 0xffff800000000000 - 0xffff807FFFFFFFFF');
console.log('This is where kernel linear mapping typically lives!\n');

const pud256TablePA = Number(pgd256Entry & PA_MASK & ~0xFFFn);
console.log(`PGD[256] points to PUD table at PA: 0x${pud256TablePA.toString(16)}\n`);

// Read the PUD table
const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pud256TablePA - GUEST_RAM_START);

// The init_task VA 0xffff800083739840 breaks down as:
// PGD index: (0xffff800083739840 >> 39) & 0x1FF = 256
// PUD index: (0xffff800083739840 >> 30) & 0x1FF = 2
// PMD index: (0xffff800083739840 >> 21) & 0x1FF = 27  
// PTE index: (0xffff800083739840 >> 12) & 0x1FF = 313
// Offset: 0xffff800083739840 & 0xFFF = 0x840

console.log('For init_task at VA 0xffff800083739840:');
console.log('  PGD[256] -> PUD[2] -> PMD[27] -> PTE[313] + 0x840\n');

// Check PUD[2]
const pud2Entry = pudBuffer.readBigUint64LE(2 * 8);
console.log(`PUD[2] entry: 0x${pud2Entry.toString(16)}`);

if ((pud2Entry & 0x3n) === 0n) {
    console.log('PUD[2] is NOT valid!');
} else if ((pud2Entry & 0x3n) === 0x1n) {
    // 1GB block
    const blockPA = Number(pud2Entry & PA_MASK & ~0x3FFFFFFFn);
    console.log('PUD[2] is a 1GB BLOCK mapping!');
    console.log(`Maps to PA range: 0x${blockPA.toString(16)} - 0x${(blockPA + 0x3FFFFFFF).toString(16)}`);
    
    // Calculate where init_task would be
    const vaOffset = 0x83739840n & 0x3FFFFFFFn;  // Offset within 1GB block
    const initTaskPA = blockPA + Number(vaOffset);
    console.log(`\ninit_task would be at PA: 0x${initTaskPA.toString(16)}`);
    
    // Check if this is in our memory file
    if (initTaskPA >= GUEST_RAM_START && initTaskPA < GUEST_RAM_START + (6 * 1024 * 1024 * 1024)) {
        console.log('✓ This PA is within our memory file!');
        
        // Try to read it
        const offset = initTaskPA - GUEST_RAM_START;
        const testBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, testBuffer, 0, 16, offset);
        console.log(`First 16 bytes at that location: ${testBuffer.toString('hex')}`);
    } else {
        console.log(`✗ This PA (0x${initTaskPA.toString(16)}) is outside our memory file range`);
        console.log(`  Memory file covers: 0x${GUEST_RAM_START.toString(16)} - 0x${(GUEST_RAM_START + 6*1024*1024*1024).toString(16)}`);
    }
} else {
    // Page table
    const pmdTablePA = Number(pud2Entry & PA_MASK & ~0xFFFn);
    console.log(`PUD[2] points to PMD table at PA: 0x${pmdTablePA.toString(16)}`);
    
    // Continue walking to find the exact PA
    console.log('\nContinuing page walk...');
}

console.log('\n=== Understanding Kernel Memory Layout ===\n');
console.log('The kernel uses HIGH virtual addresses (0xffff...) for:');
console.log('1. Linear mapping: Usually at 0xffff800000000000+');
console.log('2. vmalloc area: Dynamic kernel allocations');
console.log('3. Kernel text/data: The kernel binary itself');
console.log('');
console.log('Physical memory layout:');
console.log('0x00000000 - 0x40000000: Below RAM (kernel code/ROM/devices)');
console.log('0x40000000 - 0x1C0000000: Guest RAM (6GB) - what we have access to');
console.log('');
console.log('The init_task at PA 0x37b39840 is below 0x40000000,');
console.log('so it\'s in the kernel code/ROM area that\'s not in the memory-backend-file.');

fs.closeSync(fd);