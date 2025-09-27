#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== Finding Real init_task (PID 0, swapper) ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const stats = fs.fstatSync(fd);
const fileSize = stats.size;

console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB\n`);

// The real init_task characteristics:
// - PID = 0
// - comm = "swapper" or "swapper/0"
// - It's a kernel global variable, not dynamically allocated
// - Usually in the kernel's data section
// - Has valid kernel VAs in its linked list pointers

function isValidKernelPointer(value) {
    return value >= 0xffff000000000000n && value < 0xffffffffffffffffn;
}

function searchForInitTask(fd, fileSize) {
    console.log('Searching for init_task (PID 0, "swapper")...\n');
    
    const candidates = [];
    
    // Search regions where kernel data typically lives
    const searchRegions = [
        // Kernel data sections are often in first few hundred MB
        [0, 500 * 1024 * 1024],
        // Also check the 3-4GB region where we found other processes
        [3 * 1024 * 1024 * 1024, 4 * 1024 * 1024 * 1024],
        // And 4-5GB region
        [4 * 1024 * 1024 * 1024, 5 * 1024 * 1024 * 1024],
    ];
    
    for (const [regionStart, regionEnd] of searchRegions) {
        const start = Math.max(0, regionStart);
        const end = Math.min(fileSize, regionEnd);
        
        console.log(`Scanning region ${(start/(1024*1024*1024)).toFixed(2)}-${(end/(1024*1024*1024)).toFixed(2)}GB...`);
        
        // Scan on 8-byte boundaries (kernel structures are aligned)
        for (let offset = start; offset < end; offset += 8) {
            if (offset % (100 * 1024 * 1024) === 0) {
                process.stdout.write(`  ${(offset/(1024*1024*1024)).toFixed(2)}GB...\r`);
            }
            
            try {
                // Quick check for PID 0
                const quickBuffer = Buffer.allocUnsafe(4);
                fs.readSync(fd, quickBuffer, 0, 4, offset + PID_OFFSET);
                const pid = quickBuffer.readUint32LE(0);
                
                if (pid !== 0) continue;
                
                // Read comm field
                const commBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
                const comm = commBuffer.toString('ascii').split('\0')[0];
                
                // Check for "swapper" or "swapper/0"
                if (comm === 'swapper' || comm === 'swapper/0') {
                    // Read linked list pointers
                    const tasksBuffer = Buffer.allocUnsafe(16);
                    fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
                    const tasksNext = tasksBuffer.readBigUint64LE(0);
                    const tasksPrev = tasksBuffer.readBigUint64LE(8);
                    
                    // init_task should have valid kernel pointers
                    if (isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev)) {
                        const pa = offset + GUEST_RAM_START;
                        candidates.push({
                            pa,
                            offset,
                            pid,
                            comm,
                            tasksNext,
                            tasksPrev,
                            score: 10  // High score for matching all criteria
                        });
                        
                        console.log(`\n  Found candidate at PA 0x${pa.toString(16)}`);
                        console.log(`    PID: ${pid}, comm: "${comm}"`);
                        console.log(`    tasks.next: 0x${tasksNext.toString(16)}`);
                        console.log(`    tasks.prev: 0x${tasksPrev.toString(16)}`);
                    }
                }
                
                // Also look for idle tasks (per-CPU idle)
                if (comm && comm.startsWith('swapper/')) {
                    const tasksBuffer = Buffer.allocUnsafe(16);
                    fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
                    const tasksNext = tasksBuffer.readBigUint64LE(0);
                    const tasksPrev = tasksBuffer.readBigUint64LE(8);
                    
                    const pa = offset + GUEST_RAM_START;
                    candidates.push({
                        pa,
                        offset,
                        pid,
                        comm,
                        tasksNext,
                        tasksPrev,
                        score: tasksNext === 0n ? 1 : 5  // Lower score for per-CPU idle
                    });
                }
                
            } catch (e) {
                // Skip read errors
            }
        }
        console.log(''); // Clear progress line
    }
    
    return candidates;
}

const candidates = searchForInitTask(fd, fileSize);

console.log('\n=== ANALYSIS ===\n');

if (candidates.length === 0) {
    console.log('No init_task candidates found!');
    console.log('\nPossible reasons:');
    console.log('1. init_task might be in kernel code/data section outside our search range');
    console.log('2. Kernel might be using different offsets for task_struct fields');
    console.log('3. init_task might be optimized out or inlined');
} else {
    console.log(`Found ${candidates.length} candidates:\n`);
    
    // Sort by score
    candidates.sort((a, b) => b.score - a.score);
    
    for (const cand of candidates) {
        console.log(`PA 0x${cand.pa.toString(16)} - Score: ${cand.score}`);
        console.log(`  PID: ${cand.pid}, comm: "${cand.comm}"`);
        console.log(`  tasks.next: 0x${cand.tasksNext.toString(16)}`);
        console.log(`  tasks.prev: 0x${cand.tasksPrev.toString(16)}`);
        
        // Analysis
        if (cand.tasksNext === 0n && cand.tasksPrev === 0n) {
            console.log(`  ⚠️  Both pointers NULL - likely per-CPU idle task`);
        } else if (cand.tasksNext === cand.tasksPrev) {
            console.log(`  ⚠️  Next equals prev - single item list or corrupted`);
        } else {
            console.log(`  ✓ Looks like valid init_task!`);
        }
        console.log('');
    }
    
    // Best candidate
    const best = candidates.find(c => c.score === 10);
    if (best) {
        console.log('=== RECOMMENDED init_task ===');
        console.log(`PA: 0x${best.pa.toString(16)}`);
        console.log(`This has valid kernel pointers and correct signature.`);
        console.log(`\nTo walk process list from here:`);
        console.log(`1. Start at tasks.next: 0x${best.tasksNext.toString(16)}`);
        console.log(`2. Translate VA to PA using swapper_pg_dir`);
        console.log(`3. Read task_struct at that PA`);
        console.log(`4. Continue following next pointers`);
    }
}

fs.closeSync(fd);