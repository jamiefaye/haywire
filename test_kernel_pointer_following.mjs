#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_OFFSET = 0x438; // tasks list in task_struct

console.log('=== How Kernel Pointers Work for Straddled Task_Structs ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

function vaToPA(va) {
    return Number(va - KERNEL_VA_OFFSET);
}

function readTaskStruct(va) {
    console.log(`\nReading task_struct at VA 0x${va.toString(16)}`);
    
    const startPA = vaToPA(va);
    console.log(`  Start PA: 0x${startPA.toString(16)}`);
    
    // Check which pages this spans
    const endVA = va + BigInt(TASK_STRUCT_SIZE);
    const startPage = va & ~0xFFFn;
    const endPage = (endVA - 1n) & ~0xFFFn;
    const numPages = Number((endPage - startPage) / 0x1000n) + 1;
    
    console.log(`  Spans ${numPages} pages:`);
    
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    let bytesRead = 0;
    
    // Read page by page using VA->PA translation
    for (let pageIdx = 0; pageIdx < numPages; pageIdx++) {
        const pageVA = startPage + BigInt(pageIdx * PAGE_SIZE);
        const pagePA = vaToPA(pageVA);
        
        // Calculate offset within this page
        const startOffset = pageIdx === 0 ? Number(va & 0xFFFn) : 0;
        const endOffset = pageIdx === numPages - 1 ? 
            Number((endVA - 1n) & 0xFFFn) + 1 : PAGE_SIZE;
        const bytesToRead = endOffset - startOffset;
        
        console.log(`    Page ${pageIdx}: VA 0x${pageVA.toString(16)} -> PA 0x${pagePA.toString(16)}`);
        console.log(`      Reading bytes ${startOffset}-${endOffset} (${bytesToRead} bytes)`);
        
        const fileOffset = pagePA - GUEST_RAM_START + startOffset;
        
        if (fileOffset >= 0 && fileOffset + bytesToRead <= fs.fstatSync(fd).size) {
            try {
                fs.readSync(fd, taskData, bytesRead, bytesToRead, fileOffset);
                bytesRead += bytesToRead;
                console.log(`      ✓ Read successful`);
            } catch (e) {
                console.log(`      ✗ Read failed - page not contiguous!`);
                console.log(`      This is the straddling problem!`);
                return null;
            }
        } else {
            console.log(`      ✗ PA outside valid range`);
            return null;
        }
    }
    
    // Extract PID and comm
    const pid = taskData.readUInt32LE(PID_OFFSET);
    const comm = taskData.subarray(COMM_OFFSET, COMM_OFFSET + 16).toString('ascii').split('\0')[0];
    
    // Extract next/prev pointers
    const nextPtr = taskData.readBigUint64LE(TASKS_OFFSET);
    const prevPtr = taskData.readBigUint64LE(TASKS_OFFSET + 8);
    
    return { 
        va, 
        pid, 
        comm,
        next: nextPtr - BigInt(TASKS_OFFSET), // Adjust for list_head offset
        prev: prevPtr - BigInt(TASKS_OFFSET),
        bytesRead
    };
}

// Start with init_task
const initTaskVA = 0xffff800083739840n;
console.log(`Starting with init_task at VA 0x${initTaskVA.toString(16)}`);
console.log(`Using fixed offset: VA = PA + 0x${KERNEL_VA_OFFSET.toString(16)}`);
console.log('='.repeat(70));

const initTask = readTaskStruct(initTaskVA);
if (initTask) {
    console.log(`\nFound: PID ${initTask.pid} "${initTask.comm}"`);
    console.log(`  Next: 0x${initTask.next.toString(16)}`);
    console.log(`  Prev: 0x${initTask.prev.toString(16)}`);
    
    // Follow the chain
    console.log('\n' + '='.repeat(70));
    console.log('Following task list...\n');
    
    let current = initTask.next;
    let count = 0;
    const maxFollow = 5;
    
    while (current !== initTaskVA && count < maxFollow) {
        const task = readTaskStruct(current);
        if (!task) {
            console.log('\n✗ Failed to read task - hit non-contiguous pages!');
            console.log('This is why we only get 91% discovery!');
            break;
        }
        
        console.log(`\nFound: PID ${task.pid} "${task.comm}"`);
        current = task.next;
        count++;
    }
    
    console.log('\n' + '='.repeat(70));
    console.log('\nKey Insights:');
    console.log('-------------');
    console.log('1. The kernel uses VA pointers in task lists');
    console.log('2. We CAN translate VA->PA with fixed offset');
    console.log('3. This works for contiguous pages');
    console.log('4. BUT fails when pages are non-contiguous');
    console.log('5. The kernel knows the mapping; we do not');
    console.log('');
    console.log('For 100% discovery, we need either:');
    console.log('- SLUB metadata to find non-contiguous pages');
    console.log('- Follow kernel structures that have correct VAs');
    console.log('- Accept 91% as the practical limit');
}

fs.closeSync(fd);