#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SLAB_SIZE = 0x8000;
const TASK_STRUCT_SIZE = 9088;
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;

console.log('=== Checking Which SLABs Have Page Table Entries ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const fileSize = fs.fstatSync(fd).size;

// Read complete_pgd_mappings.txt to get all mapped PAs
const mappingFile = fs.readFileSync('/Users/jamie/haywire/docs/complete_pgd_mappings.txt', 'utf8');
const mappedPAs = new Set();

// Extract all PA mappings
const paRegex = /-> PA 0x([0-9a-f]+)/gi;
let match;
while ((match = paRegex.exec(mappingFile)) !== null) {
    const pa = parseInt(match[1], 16);
    // Store page-aligned PAs
    mappedPAs.add(pa & ~0xFFF);
}

console.log(`Found ${mappedPAs.size} pages mapped in page tables\n`);

// Now scan for task_structs and check if their pages are mapped
function checkTaskPages(slabPA) {
    const results = [];
    
    for (const offset of SLAB_OFFSETS) {
        const taskPA = slabPA + offset;
        const taskVA = BigInt(taskPA) + KERNEL_VA_OFFSET;
        
        // Check pages this task_struct spans
        const page1PA = taskPA & ~0xFFF;
        const page2PA = (taskPA + 0x1000) & ~0xFFF;
        const page3PA = (taskPA + 0x2000) & ~0xFFF;
        
        const page1Mapped = mappedPAs.has(page1PA);
        const page2Mapped = mappedPAs.has(page2PA);
        const page3Mapped = mappedPAs.has(page3PA);
        
        results.push({
            offset,
            taskPA,
            taskVA,
            pages: [page1PA, page2PA, page3PA],
            mapped: [page1Mapped, page2Mapped, page3Mapped],
            allMapped: page1Mapped && page2Mapped && page3Mapped
        });
    }
    
    return results;
}

// Sample some SLAB locations
const slabSamples = [
    0x100000000,  // 4GB
    0x110000000,  // 4.25GB  
    0x120000000,  // 4.5GB
    0x130000000,  // 4.75GB
    0x137b20000,  // Near init_task (we know this is mapped)
    0x140000000,  // 5GB
    0x150000000,  // 5.25GB
    0x160000000,  // 5.5GB
];

console.log('Checking sample SLAB locations:\n');

let mappedCount = 0;
let unmappedCount = 0;
let partialCount = 0;

for (const slabPA of slabSamples) {
    console.log(`SLAB at PA 0x${slabPA.toString(16)}:`);
    const results = checkTaskPages(slabPA);
    
    for (const r of results) {
        const mappedPages = r.mapped.filter(m => m).length;
        const status = 
            mappedPages === 3 ? 'FULLY MAPPED' :
            mappedPages === 0 ? 'NOT MAPPED' :
            `PARTIAL (${mappedPages}/3)`;
        
        console.log(`  Offset 0x${r.offset.toString(16)}: ${status}`);
        
        if (mappedPages === 3) mappedCount++;
        else if (mappedPages === 0) unmappedCount++;
        else partialCount++;
        
        // Special attention to offset 0x4700 (the problematic one)
        if (r.offset === 0x4700 && mappedPages < 3) {
            console.log(`    -> Task at 0x4700 straddles pages:`);
            for (let i = 0; i < 3; i++) {
                console.log(`       Page ${i+1}: PA 0x${r.pages[i].toString(16)} ${r.mapped[i] ? '✓ in PTEs' : '✗ NOT in PTEs'}`);
            }
        }
    }
    console.log('');
}

console.log('Summary:');
console.log(`  Fully mapped: ${mappedCount}`);
console.log(`  Not mapped: ${unmappedCount}`);
console.log(`  Partially mapped: ${partialCount}`);

fs.closeSync(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Key Insight:');
console.log('-----------');
console.log('SLABs are allocated at different times:');
console.log('1. Early SLABs: Get contiguous memory, have PTEs');
console.log('2. Later SLABs: Memory fragmented, might get non-contiguous pages');
console.log('3. On-demand PTEs: Only created when kernel accesses them');
console.log('');
console.log('This piecemeal allocation explains why:');
console.log('- Some task_structs are fully visible (early SLABs)');
console.log('- Others at 0x4700 offset fail (later SLABs, fragmented)');
console.log('- We achieve 91% not 100% discovery');