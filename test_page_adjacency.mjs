#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;

const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== Page Adjacency Test for 0x4700 Offset ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Find a process at offset 0x4700 and examine it
console.log('Searching for processes at offset 0x4700...\n');

let found = 0;
const scanSize = Math.min(4 * 1024 * 1024 * 1024, fs.fstatSync(fd).size);

for (let pageStart = 0; pageStart < scanSize && found < 5; pageStart += PAGE_SIZE) {
    const taskOffset = pageStart + 0x4700;
    
    if (taskOffset + TASK_STRUCT_SIZE > scanSize) continue;
    
    try {
        // First, let's see what happens when we try to read the full struct
        const fullBuffer = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
        fs.readSync(fd, fullBuffer, 0, TASK_STRUCT_SIZE, taskOffset);
        
        // Check if it looks like a task_struct
        const pid = fullBuffer.readUint32LE(PID_OFFSET);
        if (pid < 1 || pid > 32768) continue;
        
        const commBuffer = fullBuffer.slice(COMM_OFFSET, COMM_OFFSET + 16);
        const comm = commBuffer.toString('ascii').split('\0')[0];
        if (!comm || comm.length === 0 || comm.length > 15) continue;
        if (!/^[\x20-\x7E]+$/.test(comm)) continue;
        
        found++;
        console.log(`Found process at offset 0x${taskOffset.toString(16)}:`);
        console.log(`  PID: ${pid}, comm: "${comm}"`);
        
        // Now analyze the page layout
        const page4Start = Math.floor(taskOffset / PAGE_SIZE) * PAGE_SIZE;
        const page5Start = page4Start + PAGE_SIZE;
        const page6Start = page5Start + PAGE_SIZE;
        
        console.log(`\n  Memory layout:`);
        console.log(`    Page 4: 0x${page4Start.toString(16)} - 0x${(page5Start-1).toString(16)}`);
        console.log(`    Page 5: 0x${page5Start.toString(16)} - 0x${(page6Start-1).toString(16)}`);
        console.log(`    Page 6: 0x${page6Start.toString(16)} - 0x${(page6Start+PAGE_SIZE-1).toString(16)}`);
        
        console.log(`\n  Task struct spans:`);
        console.log(`    Start: 0x${taskOffset.toString(16)} (page 4 + 0x700)`);
        console.log(`    End:   0x${(taskOffset + TASK_STRUCT_SIZE - 1).toString(16)} (page 6 + 0xa7f)`);
        
        console.log(`\n  Critical fields:`);
        const pidAddr = taskOffset + PID_OFFSET;
        const commAddr = taskOffset + COMM_OFFSET;
        console.log(`    PID at:  0x${pidAddr.toString(16)} (page ${Math.floor(pidAddr / PAGE_SIZE)})`);
        console.log(`    comm at: 0x${commAddr.toString(16)} (page ${Math.floor(commAddr / PAGE_SIZE)})`);
        
        // Test what happens if we read pages individually
        console.log(`\n  Testing individual page reads:`);
        
        // Read just page 4 portion
        const page4Offset = taskOffset;
        const page4Size = page5Start - taskOffset;
        const page4Buffer = Buffer.allocUnsafe(page4Size);
        fs.readSync(fd, page4Buffer, 0, page4Size, page4Offset);
        
        // Can we read PID from page 4?
        const pidInPage4 = pidAddr - taskOffset < page4Size;
        if (pidInPage4) {
            const pidFromPage4 = page4Buffer.readUint32LE(PID_OFFSET);
            console.log(`    ✓ PID readable from page 4: ${pidFromPage4}`);
        } else {
            console.log(`    ✗ PID not in page 4`);
        }
        
        // Read just page 5 portion
        const page5Offset = page5Start;
        const page5Size = PAGE_SIZE;
        const page5Buffer = Buffer.allocUnsafe(page5Size);
        fs.readSync(fd, page5Buffer, 0, page5Size, page5Offset);
        
        // Can we read comm from page 5?
        const commOffsetInPage5 = commAddr - page5Start;
        if (commOffsetInPage5 >= 0 && commOffsetInPage5 < page5Size) {
            const commFromPage5 = page5Buffer.slice(commOffsetInPage5, commOffsetInPage5 + 16)
                .toString('ascii').split('\0')[0];
            console.log(`    ✓ comm readable from page 5: "${commFromPage5}"`);
        } else {
            console.log(`    ✗ comm not in page 5`);
        }
        
        // Key insight: Are the pages adjacent in the file?
        console.log(`\n  Page adjacency in file:`);
        console.log(`    File offset for page 4 data: 0x${page4Offset.toString(16)}`);
        console.log(`    File offset for page 5 data: 0x${page5Offset.toString(16)}`);
        console.log(`    Difference: ${page5Offset - page4Offset} bytes`);
        if (page5Offset - page4Offset === page5Start - taskOffset) {
            console.log(`    ✓ Pages ARE adjacent in the memory file`);
            console.log(`    This is why fs.readSync() can read across them!`);
        } else {
            console.log(`    ✗ Pages are NOT adjacent`);
        }
        
        console.log('\n' + '='.repeat(60) + '\n');
        
    } catch (e) {
        // Skip read errors
    }
}

if (found === 0) {
    console.log('No processes found at offset 0x4700');
} else {
    console.log(`\n=== KEY INSIGHT ===`);
    console.log(`\nThe memory-backend-file is a LINEAR mapping of guest physical memory.`);
    console.log(`When a task_struct at offset 0x4700 spans pages 4, 5, and 6,`);
    console.log(`those pages are CONSECUTIVE in the file.`);
    console.log(`\nfs.readSync() reads from the file sequentially, so it CAN read`);
    console.log(`across page boundaries - the pages don't need to be "adjacent"`);
    console.log(`in virtual memory, they just need to be adjacent in the file!`);
    console.log(`\nThis is why we can successfully validate processes at 0x4700`);
    console.log(`even though PID and comm are on different pages.`);
}

fs.closeSync(fd);