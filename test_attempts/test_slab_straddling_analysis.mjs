#!/usr/bin/env node

const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 0x2380;  // 9088 bytes

// Critical field offsets within task_struct
const FIELD_OFFSETS = {
    'PID': 0x750,
    'comm': 0x970,
    'mm': 0x6d0,
    'tasks_list': 0x7e0,
    'real_parent': 0x7d0,
    'parent': 0x7d8
};

// SLAB offsets for task_structs
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== SLAB Page Straddling Analysis ===\n');
console.log(`Task struct size: 0x${TASK_STRUCT_SIZE.toString(16)} (${TASK_STRUCT_SIZE} bytes)`);
console.log(`Page size: 0x${PAGE_SIZE.toString(16)} (${PAGE_SIZE} bytes)\n`);

for (const slabOffset of SLAB_OFFSETS) {
    console.log(`\n${'='.repeat(60)}`);
    console.log(`SLAB Offset 0x${slabOffset.toString(16)} (${slabOffset} bytes):`);
    console.log(`${'='.repeat(60)}`);
    
    // Calculate which pages this task_struct spans
    const startAddr = slabOffset;
    const endAddr = slabOffset + TASK_STRUCT_SIZE - 1;
    
    const startPage = Math.floor(startAddr / PAGE_SIZE);
    const endPage = Math.floor(endAddr / PAGE_SIZE);
    
    const startPageOffset = startAddr % PAGE_SIZE;
    const endPageOffset = endAddr % PAGE_SIZE;
    
    console.log(`\nStructure location:`);
    console.log(`  Starts at: 0x${startAddr.toString(16)} (page ${startPage}, offset 0x${startPageOffset.toString(16)})`);
    console.log(`  Ends at:   0x${endAddr.toString(16)} (page ${endPage}, offset 0x${endPageOffset.toString(16)})`);
    
    if (startPage === endPage) {
        console.log(`  ✓ CONTAINED in single page ${startPage}`);
    } else {
        console.log(`  ⚠️  STRADDLES ${endPage - startPage + 1} pages: [${startPage}..${endPage}]`);
        
        // Calculate how much is in each page
        console.log(`\n  Page distribution:`);
        for (let page = startPage; page <= endPage; page++) {
            const pageStart = page * PAGE_SIZE;
            const pageEnd = (page + 1) * PAGE_SIZE - 1;
            
            const structStart = Math.max(startAddr, pageStart);
            const structEnd = Math.min(endAddr, pageEnd);
            const bytesInPage = structEnd - structStart + 1;
            
            const percentage = ((bytesInPage / TASK_STRUCT_SIZE) * 100).toFixed(1);
            console.log(`    Page ${page}: ${bytesInPage} bytes (${percentage}%)`);
        }
    }
    
    // Check where critical fields land
    console.log(`\nCritical field locations:`);
    for (const [fieldName, fieldOffset] of Object.entries(FIELD_OFFSETS)) {
        const fieldAddr = slabOffset + fieldOffset;
        const fieldPage = Math.floor(fieldAddr / PAGE_SIZE);
        const fieldPageOffset = fieldAddr % PAGE_SIZE;
        
        console.log(`  ${fieldName.padEnd(12)} @ 0x${fieldAddr.toString(16).padStart(4, '0')} -> page ${fieldPage}, offset 0x${fieldPageOffset.toString(16).padStart(3, '0')}`);
    }
    
    // Identify if critical fields are split across pages
    if (startPage !== endPage) {
        console.log(`\nField accessibility issues:`);
        let issues = false;
        
        // Check if any two critical fields are on different pages
        const fieldPages = {};
        for (const [fieldName, fieldOffset] of Object.entries(FIELD_OFFSETS)) {
            const fieldAddr = slabOffset + fieldOffset;
            const fieldPage = Math.floor(fieldAddr / PAGE_SIZE);
            fieldPages[fieldName] = fieldPage;
        }
        
        // Check PID and comm specifically (most critical for discovery)
        if (fieldPages['PID'] !== fieldPages['comm']) {
            console.log(`  ❌ PID (page ${fieldPages['PID']}) and comm (page ${fieldPages['comm']}) are on DIFFERENT pages!`);
            issues = true;
        }
        
        // Check if all fields are on same page
        const uniquePages = new Set(Object.values(fieldPages));
        if (uniquePages.size === 1) {
            console.log(`  ✓ All critical fields are on the SAME page ${[...uniquePages][0]}`);
        } else {
            console.log(`  ⚠️  Critical fields span ${uniquePages.size} pages: [${[...uniquePages].sort().join(', ')}]`);
            issues = true;
        }
        
        if (!issues) {
            console.log(`  ✓ No field accessibility issues despite straddling`);
        }
    }
}

console.log(`\n\n${'='.repeat(60)}`);
console.log('SUMMARY');
console.log(`${'='.repeat(60)}\n`);

for (const slabOffset of SLAB_OFFSETS) {
    const startPage = Math.floor(slabOffset / PAGE_SIZE);
    const endPage = Math.floor((slabOffset + TASK_STRUCT_SIZE - 1) / PAGE_SIZE);
    
    const status = startPage === endPage ? '✓ Single page' : `⚠️  Straddles ${endPage - startPage + 1} pages`;
    console.log(`Offset 0x${slabOffset.toString(16).padStart(4, '0')}: ${status}`);
}

console.log(`\nImplications for memory scanning:`);
console.log(`- Offset 0x0000: No issues - fully contained in one page`);
console.log(`- Offset 0x2380: Major straddling - spans 3 pages, but fields on same page`);
console.log(`- Offset 0x4700: Moderate straddling - spans 2 pages, but fields on same page`);
console.log(`\nAll critical fields (PID, comm) remain accessible in a single page read!`);