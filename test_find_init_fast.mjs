#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== Fast Search for init_task ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

function isValidKernelPointer(value) {
    return value >= 0xffff000000000000n && value < 0xffffffffffffffffn;
}

// Based on kernel knowledge:
// - init_task is a global variable in kernel data section
// - Usually in first 100MB of kernel memory
// - On ARM64, kernel starts around PA 0x40000000
// - Kernel data section is after kernel code

console.log('Searching likely locations for init_task...\n');

const candidates = [];

// Focus on most likely areas - kernel data section
// Typically within first 100MB
const searchEnd = Math.min(100 * 1024 * 1024, fs.fstatSync(fd).size);

// Search on page boundaries (more efficient)
for (let pageStart = 0; pageStart < searchEnd; pageStart += PAGE_SIZE) {
    if (pageStart % (10 * 1024 * 1024) === 0) {
        process.stdout.write(`  Scanning ${(pageStart/(1024*1024)).toFixed(0)}MB...\r`);
    }
    
    // Check multiple offsets within each page where task_struct might start
    for (const offset of [0, 8, 16, 0x80, 0x100, 0x200, 0x400, 0x800]) {
        const checkOffset = pageStart + offset;
        
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
            
            if (comm === 'swapper' || comm === 'swapper/0') {
                // Check pointers
                const tasksBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, tasksBuffer, 0, 16, checkOffset + TASKS_LIST_OFFSET);
                const tasksNext = tasksBuffer.readBigUint64LE(0);
                const tasksPrev = tasksBuffer.readBigUint64LE(8);
                
                const pa = checkOffset + GUEST_RAM_START;
                candidates.push({
                    pa,
                    pid,
                    comm,
                    tasksNext,
                    tasksPrev,
                    hasValidPointers: isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev)
                });
                
                console.log(`\nFound candidate at PA 0x${pa.toString(16)}`);
                console.log(`  PID: ${pid}, comm: "${comm}"`);
                console.log(`  tasks.next: 0x${tasksNext.toString(16)}`);
                console.log(`  tasks.prev: 0x${tasksPrev.toString(16)}`);
                console.log(`  Valid pointers: ${isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev)}`);
            }
        } catch (e) {
            // Skip
        }
    }
}

console.log('\n');

if (candidates.length === 0) {
    console.log('No init_task found in first 100MB.');
    console.log('\nKernel init_task might be:');
    console.log('1. At a different offset (kernel version specific)');
    console.log('2. In a different memory region');
    console.log('3. Using different field offsets');
    console.log('\nNote: The systemd process (PID 1) we found earlier is NOT init_task.');
    console.log('init_task is PID 0 and is a kernel structure, not a user process.');
} else {
    console.log(`\nFound ${candidates.length} candidate(s)`);
    
    const valid = candidates.filter(c => c.hasValidPointers);
    if (valid.length > 0) {
        console.log(`\n✅ Best candidate:`);
        const best = valid[0];
        console.log(`PA: 0x${best.pa.toString(16)}`);
        console.log(`This is likely the real init_task!`);
    }
}

fs.closeSync(fd);