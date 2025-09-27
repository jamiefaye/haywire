#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const SWAPPER_PGD_PA = 0x136deb000;

console.log('=== Walking Process List from systemd (PID 1) ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// We know systemd is at PA 0x100388000 from our earlier scan
const systemdPA = 0x100388000;
const systemdOffset = systemdPA - GUEST_RAM_START;

console.log(`Starting from systemd at PA 0x${systemdPA.toString(16)}\n`);

// Verify it's systemd
try {
    const pidBuffer = Buffer.allocUnsafe(4);
    fs.readSync(fd, pidBuffer, 0, 4, systemdOffset + PID_OFFSET);
    const pid = pidBuffer.readUint32LE(0);
    
    const commBuffer = Buffer.allocUnsafe(16);
    fs.readSync(fd, commBuffer, 0, 16, systemdOffset + COMM_OFFSET);
    const comm = commBuffer.toString('ascii').split('\0')[0];
    
    console.log(`Verified: PID ${pid} "${comm}"`);
    
    if (pid !== 1 || comm !== 'systemd') {
        console.log('Warning: This might not be systemd!');
    }
    
    // Read linked list pointers
    const tasksBuffer = Buffer.allocUnsafe(16);
    fs.readSync(fd, tasksBuffer, 0, 16, systemdOffset + TASKS_LIST_OFFSET);
    const tasksNext = tasksBuffer.readBigUint64LE(0);
    const tasksPrev = tasksBuffer.readBigUint64LE(8);
    
    console.log(`tasks.next: 0x${tasksNext.toString(16)}`);
    console.log(`tasks.prev: 0x${tasksPrev.toString(16)}\n`);
    
    // These are kernel VAs - we need to walk them
    // But wait... if they're 0, we have a problem
    if (tasksNext === 0n && tasksPrev === 0n) {
        console.log('ERROR: systemd has NULL linked list pointers!');
        console.log('This suggests the process list is not properly linked.');
        console.log('');
        console.log('Alternative: Let\'s check if adjacent task_structs are linked...');
        
        // Check the task_struct at the next SLAB position
        const nextSlabOffset = systemdOffset + TASK_STRUCT_SIZE;
        const nextPidBuffer = Buffer.allocUnsafe(4);
        fs.readSync(fd, nextPidBuffer, 0, 4, nextSlabOffset + PID_OFFSET);
        const nextPid = nextPidBuffer.readUint32LE(0);
        
        if (nextPid > 0 && nextPid < 32768) {
            const nextCommBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, nextCommBuffer, 0, 16, nextSlabOffset + COMM_OFFSET);
            const nextComm = nextCommBuffer.toString('ascii').split('\0')[0];
            console.log(`\nFound adjacent task_struct: PID ${nextPid} "${nextComm}"`);
            console.log('Task_structs are in SLAB but might not be linked via tasks list.');
        }
    } else if (tasksNext !== 0n || tasksPrev !== 0n) {
        console.log('systemd has linked list pointers!');
        console.log('We could walk the list if we can translate these VAs to PAs.');
        console.log('This would require using swapper_pg_dir for translation.');
    }
    
} catch (e) {
    console.log(`Error: ${e.message}`);
}

console.log('\n=== Analysis ===\n');
console.log('Current findings:');
console.log('1. We can find individual task_structs by scanning (91% success)');
console.log('2. We found 8 per-CPU idle tasks (swapper/N) with NULL pointers');
console.log('3. The main init_task is elusive - might use different structure');
console.log('4. Task_structs might not always be linked via tasks list');
console.log('');
console.log('Our scanning approach remains the most reliable method');
console.log('for process discovery given these constraints.');

fs.closeSync(fd);