#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== Accessing init_task at PA 0x37b39840 ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const stats = fs.fstatSync(fd);
const fileSize = stats.size;

console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB`);
console.log(`File covers PA range: 0x${GUEST_RAM_START.toString(16)} - 0x${(GUEST_RAM_START + fileSize).toString(16)}\n`);

const initTaskPA = 0x37b39840;

console.log(`init_task PA: 0x${initTaskPA.toString(16)}`);
console.log(`RAM starts at: 0x${GUEST_RAM_START.toString(16)}`);

if (initTaskPA < GUEST_RAM_START) {
    console.log(`\nPA 0x${initTaskPA.toString(16)} is ${(GUEST_RAM_START - initTaskPA).toString(16)} bytes BELOW RAM start`);
    console.log('\nThis is in the memory region BEFORE RAM:');
    console.log('- 0x00000000 - 0x08000000: Flash/ROM (kernel image)');
    console.log('- 0x08000000 - 0x40000000: MMIO devices');
    console.log('');
    console.log('The memory-backend-file starts at PA 0x40000000 (1GB mark)');
    console.log('So init_task at PA 0x37b39840 is NOT in the file.');
    console.log('');
    console.log('This makes sense because:');
    console.log('1. init_task is a kernel global variable');
    console.log('2. It\'s compiled into the kernel binary');
    console.log('3. The kernel binary is loaded in Flash/ROM area');
    console.log('4. QEMU\'s memory-backend-file only includes RAM, not ROM');
} else {
    // It would be in the file
    const offset = initTaskPA - GUEST_RAM_START;
    console.log(`\nFile offset would be: 0x${offset.toString(16)}`);
    
    if (offset < fileSize) {
        console.log('Reading from file...');
        try {
            const pidBuffer = Buffer.allocUnsafe(4);
            fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
            const pid = pidBuffer.readUint32LE(0);
            
            const commBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
            const comm = commBuffer.toString('ascii').split('\0')[0];
            
            console.log(`  PID: ${pid}`);
            console.log(`  comm: "${comm}"`);
        } catch (e) {
            console.log(`Error reading: ${e.message}`);
        }
    }
}

console.log('\n=== Where ARE kernel structures then? ===\n');
console.log('Most kernel structures we care about ARE in RAM:');
console.log('1. task_structs for processes - in SLAB/SLUB (RAM)');
console.log('2. mm_structs - dynamically allocated (RAM)');
console.log('3. Page tables - allocated from RAM');
console.log('4. Most kernel data structures - in RAM');
console.log('');
console.log('What\'s NOT in RAM (and thus not in memory-backend-file):');
console.log('1. Kernel code (.text section)');
console.log('2. Read-only kernel data (.rodata)');
console.log('3. Some global variables like init_task');
console.log('4. Boot-time structures');
console.log('');
console.log('This is why we found 91% of processes - they\'re dynamically');
console.log('allocated in RAM. The missing ones might be:');
console.log('- Kernel threads with task_structs in ROM/kernel data');
console.log('- Or in fragmented/non-contiguous SLAB pages');

fs.closeSync(fd);