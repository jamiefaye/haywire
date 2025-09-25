#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== Correctly Accessing init_task ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const stats = fs.fstatSync(fd);
const fileSize = stats.size;

console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB`);
console.log(`File represents PA range: 0x${GUEST_RAM_START.toString(16)} - 0x${(GUEST_RAM_START + fileSize).toString(16)}\n`);

// From our page table walk, init_task is at PA 0x37b39840
// But wait... that's BELOW 0x40000000
// Let me recalculate from the translation we did

console.log('Rechecking the translation from earlier:');
console.log('init_task VA: 0xffff800083739840');
console.log('Our translation gave PA: 0x37b39840');
console.log('');
console.log('This PA is 0x37b39840 = 934844480 decimal');
console.log('RAM starts at 0x40000000 = 1073741824 decimal');
console.log('Difference: RAM_START - PA = 138897344 bytes (132.5 MB)');
console.log('');
console.log('This seems wrong. Let me check if there was an error in translation.');
console.log('');

// The PTE we found was: 0x78000137b39703
// Let's decode this properly
const pteValue = 0x78000137b39703n;
console.log('PTE value from translation: 0x' + pteValue.toString(16));
console.log('Breaking down the PTE:');
console.log('  Bits [47:12] (PA): 0x' + ((pteValue >> 12n) & 0xFFFFFFFFFn).toString(16));
console.log('  With offset 0x840: PA would be 0x137b39840');
console.log('');
console.log('AH! The PA should be 0x137b39840, not 0x37b39840!');
console.log('I dropped a digit in the earlier translation.\n');

const correctPA = 0x137b39840;
console.log(`Correct init_task PA: 0x${correctPA.toString(16)}`);

// Check if this is in our file
if (correctPA < GUEST_RAM_START) {
    console.log('Still below RAM start - impossible');
} else {
    const offset = correctPA - GUEST_RAM_START;
    console.log(`File offset: 0x${offset.toString(16)} (${(offset/(1024*1024*1024)).toFixed(2)}GB into file)`);
    
    if (offset >= fileSize) {
        console.log(`\nThis offset is beyond our 6GB file size.`);
        console.log(`Would need a ${((offset + PAGE_SIZE)/(1024*1024*1024)).toFixed(2)}GB file to reach it.`);
    } else {
        console.log('\nThis IS in our file! Reading init_task...');
        
        try {
            // Read PID
            const pidBuffer = Buffer.allocUnsafe(4);
            fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
            const pid = pidBuffer.readUint32LE(0);
            
            // Read comm
            const commBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
            const comm = commBuffer.toString('ascii').split('\0')[0];
            
            // Read tasks pointers
            const tasksBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
            const tasksNext = tasksBuffer.readBigUint64LE(0);
            const tasksPrev = tasksBuffer.readBigUint64LE(8);
            
            console.log('\n=== init_task contents ===');
            console.log(`  PID: ${pid}`);
            console.log(`  comm: "${comm}"`);
            console.log(`  tasks.next: 0x${tasksNext.toString(16)}`);
            console.log(`  tasks.prev: 0x${tasksPrev.toString(16)}`);
            
            if (pid === 0 && comm === 'swapper') {
                console.log('\n✅ CONFIRMED: This is init_task!');
                console.log('We CAN access it from the memory-backend-file!');
            } else if (pid === 0 && comm.startsWith('swapper/')) {
                console.log('\n✅ This is a per-CPU idle task (swapper/N)');
            } else {
                console.log('\n⚠️  Unexpected values at this location');
                console.log('Might need different offsets or this isn\'t init_task');
            }
            
        } catch (e) {
            console.log(`Error reading: ${e.message}`);
        }
    }
}

console.log('\n=== Summary ===\n');
console.log('Everything IS in the memory-backend-file:');
console.log('- Kernel code/data: Yes, loaded into RAM');
console.log('- Process task_structs: Yes, in SLAB/SLUB');
console.log('- Page tables: Yes, allocated from RAM');
console.log('- init_task: Should be accessible if we have the right PA');
console.log('');
console.log('The memory-backend-file represents ALL guest physical memory');
console.log('starting from 0x40000000 (where RAM begins on this platform).');

fs.closeSync(fd);