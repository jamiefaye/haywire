#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;

console.log('=== Finding How SLAB Virtual Addresses Are Mapped ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

function readPage(fd, pa) {
    const buffer = Buffer.allocUnsafe(PAGE_SIZE);
    const offset = pa - GUEST_RAM_START;
    
    if (offset < 0 || offset + PAGE_SIZE > fs.fstatSync(fd).size) {
        return null;
    }
    
    try {
        fs.readSync(fd, buffer, 0, PAGE_SIZE, offset);
        return buffer;
    } catch (e) {
        return null;
    }
}

// Let's check if SLAB VAs are in the linear map region
// Linear map typically starts at 0xffff800000000000

const initTaskVA = 0xffff800083739840n; // init_task VA we know
const initTaskPA = 0x137b39840; // Corresponding PA

console.log('Known mapping:');
console.log(`  init_task VA: 0x${initTaskVA.toString(16)}`);
console.log(`  init_task PA: 0x${initTaskPA.toString(16)}`);
console.log(`  Offset: 0x${KERNEL_VA_OFFSET.toString(16)}`);
console.log('');

// Now let's check the page tables for the linear map region
// This should be in PGD[256] (0xffff800000000000)

const pgdBuffer = readPage(fd, SWAPPER_PGD_PA);
const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);

console.log(`PGD[256] entry: 0x${pgd256.toString(16)}`);

if ((pgd256 & 0x3n) === 0x3n) {
    const pudTablePA = Number(pgd256 & PA_MASK & ~0xFFFn);
    console.log(`PUD table at PA: 0x${pudTablePA.toString(16)}`);
    
    const pudBuffer = readPage(fd, pudTablePA);
    
    // The init_task VA 0xffff800083739840 breaks down as:
    // PGD index: 256 (0xffff800000000000 base)
    // PUD index: (0x83739840 >> 30) & 0x1FF = 2
    // PMD index: (0x83739840 >> 21) & 0x1FF = 27
    // PTE index: (0x83739840 >> 12) & 0x1FF = 0x339
    
    const pudIdx = Number((initTaskVA >> 30n) & 0x1FFn);
    console.log(`\nLooking for init_task in page tables:`);
    console.log(`  PUD index: ${pudIdx}`);
    
    const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
    console.log(`  PUD[${pudIdx}] entry: 0x${pudEntry.toString(16)}`);
    
    if (pudEntry === 0n) {
        console.log('  -> PUD entry is ZERO! No page table here!');
        console.log('\n*** This proves SLAB is NOT in the page tables! ***');
    } else if ((pudEntry & 0x3n) === 0x1n) {
        // 1GB block mapping
        const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
        console.log(`  -> 1GB block at PA 0x${blockPA.toString(16)}`);
        console.log('\n*** This is a HUGE PAGE mapping! ***');
        console.log('The entire 1GB region uses a fixed offset.');
    } else if ((pudEntry & 0x3n) === 0x3n) {
        // PMD table
        const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
        console.log(`  -> PMD table at PA 0x${pmdTablePA.toString(16)}`);
        
        const pmdBuffer = readPage(fd, pmdTablePA);
        const pmdIdx = Number((initTaskVA >> 21n) & 0x1FFn);
        console.log(`  PMD index: ${pmdIdx}`);
        
        const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
        console.log(`  PMD[${pmdIdx}] entry: 0x${pmdEntry.toString(16)}`);
        
        if (pmdEntry === 0n) {
            console.log('  -> PMD entry is ZERO! No page table here!');
            console.log('\n*** SLAB region is NOT mapped in page tables! ***');
        }
    }
}

console.log('\n' + '='.repeat(70) + '\n');
console.log('Analysis:');
console.log('---------');
console.log('If SLAB VAs are not in the page tables, then either:');
console.log('');
console.log('1. **Hardware Linear Mapping** (most likely):');
console.log('   ARM64 MMU has a feature where certain VA ranges');
console.log('   automatically map to PA = VA - fixed_offset');
console.log('   No page tables needed!');
console.log('');
console.log('2. **Identity/Direct Mapping at Boot**:');
console.log('   Set up early in boot before page tables');
console.log('   Uses TTBR registers directly');
console.log('');
console.log('3. **Huge Pages (1GB/2MB blocks)**:');
console.log('   Entire regions mapped with single PUD/PMD entry');
console.log('   But still assumes contiguous physical memory');
console.log('');
console.log('The key insight: If pages are NON-CONTIGUOUS,');
console.log('they MUST have individual PTE entries somewhere!');
console.log('But we\'re not finding them...');

fs.closeSync(fd);