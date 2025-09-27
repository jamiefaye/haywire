#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== Finding the Main init_task (not per-CPU idle) ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const fileSize = fs.fstatSync(fd).size;

console.log(`Searching for init_task with PID 0 and valid linked list pointers...\n`);

// We found swapper/0 at PA 0x137b39840 (file offset 0xf7b39840)
// But it has NULL pointers. Let's search nearby and other locations

const candidates = [];

// Search regions where init_task might be
const searchRegions = [
    // Near where we found swapper/0
    [0xf7b30000, 0xf7b50000],
    // Earlier in that GB
    [0xf7000000, 0xf8000000],
    // Common kernel data areas (3-5GB range)
    [0xc0000000, 0x140000000],
];

function isValidKernelPointer(value) {
    return value >= 0xffff000000000000n && value < 0xffffffffffffffffn;
}

for (const [regionStart, regionEnd] of searchRegions) {
    const start = Math.max(0, regionStart);
    const end = Math.min(fileSize, regionEnd);
    
    if (start >= end) continue;
    
    console.log(`Scanning region 0x${start.toString(16)}-0x${end.toString(16)}...`);
    
    // Scan on page boundaries
    for (let offset = start; offset < end; offset += PAGE_SIZE) {
        if (offset % (100 * 1024 * 1024) === 0) {
            process.stdout.write(`  ${((offset - start)/(1024*1024)).toFixed(0)}MB scanned\r`);
        }
        
        // Check a few offsets within each page
        for (const pageOffset of [0, 0x840, 0x1000, 0x1840]) {
            const checkOffset = offset + pageOffset;
            if (checkOffset + TASK_STRUCT_SIZE > fileSize) continue;
            
            try {
                // Quick check for PID 0
                const pidBuffer = Buffer.allocUnsafe(4);
                fs.readSync(fd, pidBuffer, 0, 4, checkOffset + PID_OFFSET);
                const pid = pidBuffer.readUint32LE(0);
                
                if (pid !== 0) continue;
                
                // Check comm
                const commBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, commBuffer, 0, 16, checkOffset + COMM_OFFSET);
                const comm = commBuffer.toString('ascii').split('\0')[0];
                
                if (!comm || !comm.startsWith('swapper')) continue;
                
                // Check linked list pointers
                const tasksBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, tasksBuffer, 0, 16, checkOffset + TASKS_LIST_OFFSET);
                const tasksNext = tasksBuffer.readBigUint64LE(0);
                const tasksPrev = tasksBuffer.readBigUint64LE(8);
                
                const pa = checkOffset + GUEST_RAM_START;
                
                candidates.push({
                    pa,
                    offset: checkOffset,
                    pid,
                    comm,
                    tasksNext,
                    tasksPrev,
                    hasValidPointers: isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev),
                    bothNull: tasksNext === 0n && tasksPrev === 0n
                });
                
                if (candidates.length === 1 || (tasksNext !== 0n || tasksPrev !== 0n)) {
                    console.log(`\n  Found: PID ${pid} "${comm}" at PA 0x${pa.toString(16)}`);
                    console.log(`    tasks.next: 0x${tasksNext.toString(16)}`);
                    console.log(`    tasks.prev: 0x${tasksPrev.toString(16)}`);
                }
                
            } catch (e) {
                // Skip
            }
        }
    }
    console.log(''); // Clear progress line
}

console.log(`\nFound ${candidates.length} swapper candidates\n`);

// Analyze what we found
const withValidPointers = candidates.filter(c => c.hasValidPointers);
const withNullPointers = candidates.filter(c => c.bothNull);
const withMixedPointers = candidates.filter(c => !c.hasValidPointers && !c.bothNull);

console.log(`With valid kernel pointers: ${withValidPointers.length}`);
console.log(`With NULL pointers (idle tasks): ${withNullPointers.length}`);
console.log(`With mixed/invalid pointers: ${withMixedPointers.length}\n`);

if (withValidPointers.length > 0) {
    console.log('=== Main init_task Candidate ===');
    const best = withValidPointers[0];
    console.log(`PA: 0x${best.pa.toString(16)}`);
    console.log(`PID: ${best.pid}, comm: "${best.comm}"`);
    console.log(`tasks.next: 0x${best.tasksNext.toString(16)}`);
    console.log(`tasks.prev: 0x${best.tasksPrev.toString(16)}`);
    console.log('\n✅ This is likely the main init_task!');
    console.log('The linked list pointers point to other task_structs in the system.');
} else {
    console.log('No init_task with valid linked list pointers found.');
    console.log('\nPossible reasons:');
    console.log('1. The main init_task might use different offsets');
    console.log('2. It might be at an unexpected location');
    console.log('3. We found per-CPU idle tasks but not the main one');
    
    if (withNullPointers.length > 0) {
        console.log(`\nWe did find ${withNullPointers.length} idle tasks (swapper/N with NULL pointers)`);
        console.log('These are per-CPU idle threads, not the main init_task.');
    }
}

fs.closeSync(fd);