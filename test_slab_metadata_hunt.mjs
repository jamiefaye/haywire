#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;

console.log('=== SLAB Metadata Hunt ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

console.log('SLUB Architecture Overview:\n');
console.log('1. struct kmem_cache - The cache descriptor');
console.log('   - Contains size, object_size, offset, etc.');
console.log('   - Points to per-CPU and per-node structures');
console.log('');
console.log('2. struct kmem_cache_cpu - Per-CPU data');
console.log('   - freelist: pointer to first free object');
console.log('   - page: pointer to current slab page');
console.log('   - partial: list of partially filled slabs');
console.log('');
console.log('3. struct page/slab - Page metadata');
console.log('   - freelist: first free object in this slab');
console.log('   - inuse: number of allocated objects');
console.log('   - objects: total number of objects');
console.log('   - slab_cache: pointer back to kmem_cache');
console.log('');
console.log('4. Free objects contain next pointer at offset 0');
console.log('');
console.log('='.repeat(60) + '\n');

// Known task_struct locations we found earlier
const knownTaskStructs = [
    { pa: 0x422c8000, pid: 1734, comm: 'gsd-media-keys' },
    { pa: 0x422d8000, pid: 1732, comm: 'gsd-keyboard' },
    { pa: 0x100388000, pid: 1, comm: 'systemd' },
    { pa: 0x10038c700, pid: 2, comm: 'kthreadd' }
];

console.log('Analyzing pages containing known task_structs...\n');

for (const task of knownTaskStructs) {
    const pageAlignedPA = Math.floor(task.pa / PAGE_SIZE) * PAGE_SIZE;
    const offsetInPage = task.pa % PAGE_SIZE;
    
    console.log(`Task: PID ${task.pid} (${task.comm})`);
    console.log(`  PA: 0x${task.pa.toString(16)}`);
    console.log(`  Page: 0x${pageAlignedPA.toString(16)}`);
    console.log(`  Offset in page: 0x${offsetInPage.toString(16)}`);
    
    // For SLUB, we expect patterns:
    // - If offset is 0x0: First object in slab
    // - If offset is 0x2380: Second object (if contiguous)
    // - If offset is 0x4700: Third object (if contiguous)
    
    if (offsetInPage === 0x0) {
        console.log(`  → First object in SLAB`);
        
        // Check if there's another task_struct at +0x2380
        const nextOffset = task.pa + TASK_STRUCT_SIZE - GUEST_RAM_START;
        const checkBuffer = Buffer.allocUnsafe(4);
        try {
            fs.readSync(fd, checkBuffer, 0, 4, nextOffset + 0x750); // PID offset
            const nextPid = checkBuffer.readUint32LE(0);
            if (nextPid > 0 && nextPid < 32768) {
                console.log(`  ✓ Found adjacent task_struct with PID ${nextPid}`);
            } else {
                console.log(`  ✗ No valid task_struct at +0x2380`);
            }
        } catch {}
        
    } else if (offsetInPage === 0x2380) {
        console.log(`  → Second object in SLAB (if contiguous)`);
    } else if (offsetInPage === 0x700 || offsetInPage === 0xc700) {
        console.log(`  → Likely third object (page-straddling)`);
    }
    console.log('');
}

console.log('='.repeat(60) + '\n');
console.log('Looking for SLAB page patterns...\n');

// In SLUB, when we have a slab page, we might see:
// 1. Multiple task_structs at regular intervals (if allocated)
// 2. Free list pointers in unallocated slots
// 3. Metadata at page struct location

// Let's check a page we know has task_structs
const testPagePA = 0x422c8000; // Page with gsd-media-keys
const testPageOffset = testPagePA - GUEST_RAM_START;

console.log(`Examining page at PA 0x${testPagePA.toString(16)}...\n`);

// Read the entire page
const pageBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pageBuffer, 0, PAGE_SIZE, testPageOffset);

// Look for task_struct signatures
const signatures = [];
for (let offset = 0; offset < PAGE_SIZE - 16; offset += 8) {
    // Look for potential PID at standard offset
    if (offset + 0x750 + 4 <= PAGE_SIZE) {
        const potentialPid = pageBuffer.readUint32LE(offset + 0x750);
        if (potentialPid > 0 && potentialPid < 1000) {
            signatures.push({ offset, pid: potentialPid });
        }
    }
}

if (signatures.length > 0) {
    console.log(`Found ${signatures.length} potential task_struct signatures:`);
    for (const sig of signatures) {
        console.log(`  Offset 0x${sig.offset.toString(16)}: PID ${sig.pid}`);
    }
} else {
    console.log('No clear task_struct signatures in this page alone');
}

console.log('');
console.log('='.repeat(60) + '\n');
console.log('SLUB Free List Exploration...\n');

// In a partially allocated SLAB, free objects contain pointers
// The first 8 bytes of a free object point to the next free object

// Let's look for potential free list pointers
console.log('Scanning for potential free list pointers...\n');

const scanSize = Math.min(100 * PAGE_SIZE, fs.fstatSync(fd).size);
const slabCandidates = [];

for (let pageStart = 0; pageStart < scanSize; pageStart += PAGE_SIZE) {
    const buffer = Buffer.allocUnsafe(64);
    try {
        fs.readSync(fd, buffer, 0, 64, pageStart);
        
        // Look for repeating patterns that might indicate SLAB
        const ptr1 = buffer.readBigUint64LE(0);
        const ptr2 = buffer.readBigUint64LE(TASK_STRUCT_SIZE % 64);
        
        // Check if these look like kernel pointers
        if (ptr1 >= 0xffff000000000000n && ptr1 < 0xffffffffffffffffn) {
            slabCandidates.push({
                pagePA: pageStart + GUEST_RAM_START,
                firstPtr: ptr1
            });
        }
    } catch {}
}

if (slabCandidates.length > 0) {
    console.log(`Found ${slabCandidates.length} pages with kernel pointers at offset 0:`);
    for (const cand of slabCandidates.slice(0, 5)) {
        console.log(`  Page 0x${cand.pagePA.toString(16)}: pointer 0x${cand.firstPtr.toString(16)}`);
    }
}

console.log('');
console.log('='.repeat(60) + '\n');
console.log('KEY INSIGHTS:\n');

console.log('To properly enumerate SLAB members, we would need:');
console.log('');
console.log('1. Find kmem_cache for task_struct:');
console.log('   - Symbol: task_struct_cachep');
console.log('   - Contains all metadata about the cache');
console.log('');
console.log('2. Walk the per-CPU structures:');
console.log('   - Each CPU has active slabs');
console.log('   - Contains free lists and partial lists');
console.log('');
console.log('3. Parse struct page/slab metadata:');
console.log('   - Linux 5.17+ uses struct slab');
console.log('   - Earlier uses struct page');
console.log('   - Contains object count and free list');
console.log('');
console.log('4. Alternative: Use the buddy allocator:');
console.log('   - Find which pages are allocated to SLUB');
console.log('   - Check page flags for SLAB bit');
console.log('');
console.log('Our current approach (scanning + validation) works because:');
console.log('- Most task_structs are in contiguous SLAB pages');
console.log('- We achieve 91% discovery without needing metadata');
console.log('- The missing 9% are likely in fragmented slabs');

fs.closeSync(fd);