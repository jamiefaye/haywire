#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;

console.log('=== SLAB Memory Mapping Analysis ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

console.log('Understanding kernel memory mappings:\n');
console.log('1. LINEAR MAPPING (PGD[0]):');
console.log('   - Maps first 4GB of physical RAM linearly');
console.log('   - VA range: 0x0 - 0x100000000');
console.log('   - PA range: 0x40000000 - 0x140000000');
console.log('   - Simple translation: PA = VA + 0x40000000');
console.log('');

console.log('2. VMALLOC AREA (PGD[256]):');
console.log('   - Non-contiguous mappings for vmalloc allocations');
console.log('   - VA range: 0xffff000000000000+');
console.log('   - Used for: module memory, large allocations');
console.log('');

console.log('3. FIXMAP/KASAN/etc (PGD[507]):');
console.log('   - Special kernel mappings');
console.log('');

console.log('Key question: Where do SLAB/SLUB allocations live?\n');
console.log('='.repeat(60) + '\n');

// Let's check where our found task_structs are in physical memory
const foundTaskStructs = [
    { pa: 0x422c8000, pid: 1734, comm: 'gsd-media-keys' },
    { pa: 0x100388000, pid: 1, comm: 'systemd' },
    { pa: 0x10038c700, pid: 2, comm: 'kthreadd' }
];

console.log('Analyzing known task_struct locations:\n');

for (const task of foundTaskStructs) {
    console.log(`PID ${task.pid} (${task.comm}):`);
    console.log(`  Physical Address: 0x${task.pa.toString(16)}`);
    
    // Check if it's in the linear mapping range
    if (task.pa >= GUEST_RAM_START && task.pa < GUEST_RAM_START + 0x100000000) {
        const vaLinear = task.pa - GUEST_RAM_START;
        console.log(`  ✓ In LINEAR mapping range`);
        console.log(`    Would be at VA 0x${vaLinear.toString(16)} in linear map`);
        
        // Which part of the linear range?
        const offsetFromStart = task.pa - GUEST_RAM_START;
        const gb = Math.floor(offsetFromStart / (1024 * 1024 * 1024));
        console.log(`    Located in GB ${gb} of RAM`);
    } else if (task.pa >= GUEST_RAM_START + 0x100000000 && task.pa < GUEST_RAM_START + 0x180000000) {
        console.log(`  ✗ OUTSIDE linear mapping range (in high RAM)`);
        console.log(`    This is in the 4-6GB range`);
        console.log(`    Must be accessed via different mapping`);
    }
    console.log('');
}

console.log('='.repeat(60) + '\n');
console.log('SLAB/SLUB allocation patterns:\n');

console.log('1. For task_structs in 0-4GB range (PA 0x40000000-0x140000000):');
console.log('   - These ARE in the linear mapping');
console.log('   - Kernel accesses via VA = PA - 0x40000000');
console.log('   - Example: PA 0x422c8000 -> VA 0x22c8000');
console.log('');

console.log('2. For task_structs in 4-6GB range (PA 0x140000000-0x180000000):');
console.log('   - These are OUTSIDE the linear mapping');
console.log('   - Must be mapped elsewhere (likely vmalloc area via PGD[256])');
console.log('   - Or accessed via temporary mappings (kmap)');
console.log('');

console.log('3. SLUB behavior:');
console.log('   - Tries to allocate order-2 pages (16KB = 4 pages) for 9KB objects');
console.log('   - Under memory pressure, falls back to order-0 (single pages)');
console.log('   - Pages from buddy allocator may not be contiguous');
console.log('');

// Now let's look at the swapper_pg_dir to understand the mappings
console.log('='.repeat(60) + '\n');
console.log('Reading swapper_pg_dir entries...\n');

const pgdOffset = SWAPPER_PGD_PA - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, pgdOffset);

// Check key PGD entries
const keyEntries = [0, 256, 507, 511];
for (const index of keyEntries) {
    const entry = pgdBuffer.readBigUint64LE(index * 8);
    if (entry !== 0n) {
        console.log(`PGD[${index}]: 0x${entry.toString(16)}`);
        
        const valid = (entry & 0x3n) !== 0n;
        const tableAddr = Number(entry & 0x0000FFFFFFFFFFFFn & ~0xFFFn);
        
        if (valid) {
            console.log(`  Points to table at PA 0x${tableAddr.toString(16)}`);
            
            if (index === 0) {
                console.log(`  This is the LINEAR MAPPING table`);
            } else if (index === 256) {
                console.log(`  This is likely VMALLOC area`);
            } else if (index === 507) {
                console.log(`  This is likely FIXMAP/special mappings`);
            }
        }
        console.log('');
    }
}

console.log('='.repeat(60) + '\n');
console.log('KEY INSIGHTS:\n');

console.log('1. SLAB pages are allocated from the buddy allocator');
console.log('2. They can be anywhere in physical memory');
console.log('3. If in 0-4GB range: accessed via linear mapping');
console.log('4. If in 4-6GB range: need vmalloc or temporary mappings');
console.log('5. Adjacent SLAB objects may use non-contiguous physical pages');
console.log('');
console.log('To trace SLAB continuity:');
console.log('- We cannot rely on physical adjacency');
console.log('- The kernel uses virtual addresses internally');
console.log('- SLUB manages free lists with pointers, not assumptions of contiguity');
console.log('- Our 91% discovery rate works because most task_structs fit in single');
console.log('  SLUB allocations that got contiguous pages from buddy allocator');

fs.closeSync(fd);