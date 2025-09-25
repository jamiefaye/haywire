#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;

console.log('=== Examining init_struct_pid ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

function vaToPA(va) {
    return Number(va - KERNEL_VA_OFFSET);
}

function readMemory(fd, pa, size) {
    const buffer = Buffer.allocUnsafe(size);
    const offset = pa - GUEST_RAM_START;
    
    if (offset < 0 || offset + size > fs.fstatSync(fd).size) {
        return null;
    }
    
    try {
        fs.readSync(fd, buffer, 0, size, offset);
        return buffer;
    } catch (e) {
        return null;
    }
}

// Get init_struct_pid address
const result = await new Promise((resolve) => {
    exec(`ssh vm "sudo grep 'init_struct_pid' /proc/kallsyms | head -1"`, (error, stdout) => {
        if (error) {
            resolve(null);
            return;
        }
        const [addr] = stdout.trim().split(' ');
        resolve(BigInt('0x' + addr));
    });
});

if (!result) {
    console.log('Failed to get init_struct_pid address');
    process.exit(1);
}

const initStructPidVA = result;
console.log(`init_struct_pid at VA: 0x${initStructPidVA.toString(16)}`);

const initStructPidPA = vaToPA(initStructPidVA);
console.log(`init_struct_pid at PA: 0x${initStructPidPA.toString(16)}`);

// Read struct pid
const pidBuffer = readMemory(fd, initStructPidPA, 0x200);
if (!pidBuffer) {
    console.log('Failed to read init_struct_pid');
    process.exit(1);
}

console.log('\nstruct pid contents:');
console.log('--------------------');

// struct pid {
//     refcount_t count;        // 0x0
//     unsigned int level;      // 0x4  
//     spinlock_t lock;        // 0x8
//     struct hlist_head tasks[PIDTYPE_MAX]; // 0x10
//     struct hlist_head inodes;  // varies
//     wait_queue_head_t wait_pidfd; // varies
//     struct rcu_head rcu;     // varies
//     struct upid numbers[];   // varies
// };

const refcount = pidBuffer.readUint32LE(0x0);
const level = pidBuffer.readUint32LE(0x4);

console.log(`refcount: ${refcount}`);
console.log(`level: ${level}`);

// Check tasks array (PIDTYPE_MAX = 4)
console.log('\ntasks[] array (hlist_head pointers):');
for (let i = 0; i < 4; i++) {
    const taskPtr = pidBuffer.readBigUint64LE(0x10 + i * 8);
    console.log(`  tasks[${i}]: 0x${taskPtr.toString(16)}`);
    
    if (taskPtr !== 0n && taskPtr > 0xffff000000000000n) {
        // This points to a pid_link in a task_struct
        // struct pid_link is at offset ~0x4e8 in task_struct
        const taskStructVA = taskPtr - 0x4e8n;
        console.log(`    -> Likely task_struct at VA: 0x${taskStructVA.toString(16)}`);
        
        const taskPA = vaToPA(taskStructVA);
        const taskBuffer = readMemory(fd, taskPA, 0x1000);
        if (taskBuffer) {
            const pid = taskBuffer.readUint32LE(0x750);
            const comm = taskBuffer.subarray(0x970, 0x980).toString('ascii').split('\0')[0];
            console.log(`       PID ${pid}: "${comm}"`);
        }
    }
}

// Check upid structure at end
console.log('\nupid structure (PID number):');
const upidOffset = 0x30; // After tasks[], inodes, wait_pidfd, rcu
const nr = pidBuffer.readUint32LE(upidOffset);
const nsPtr = pidBuffer.readBigUint64LE(upidOffset + 8);
console.log(`  nr (PID number): ${nr}`);
console.log(`  ns (namespace ptr): 0x${nsPtr.toString(16)}`);

// Now let's scan for more struct pid patterns
console.log('\n' + '='.repeat(70) + '\n');
console.log('Scanning for struct pid patterns in memory...');

let foundPids = 0;
const maxScan = 1000; // Scan 1000 pages

for (let pageNum = 0; pageNum < maxScan; pageNum++) {
    const pa = GUEST_RAM_START + 0x130000000 + pageNum * 0x1000; // Start near kernel area
    const pageBuffer = readMemory(fd, pa, 0x1000);
    if (!pageBuffer) continue;
    
    // Look for struct pid pattern
    for (let offset = 0; offset < 0x1000 - 0x40; offset += 8) {
        const refcount = pageBuffer.readUint32LE(offset);
        const level = pageBuffer.readUint32LE(offset + 4);
        
        // Heuristic: valid struct pid has refcount 1-100 and level 0-2
        if (refcount > 0 && refcount < 100 && level <= 2) {
            const taskPtr = pageBuffer.readBigUint64LE(offset + 0x10);
            
            // Check if tasks[0] looks like a valid kernel pointer
            if (taskPtr > 0xffff800000000000n && taskPtr < 0xffffffffffffffffn) {
                foundPids++;
                if (foundPids <= 5) {
                    console.log(`  Found potential struct pid at PA 0x${(pa + offset).toString(16)}`);
                    console.log(`    refcount=${refcount}, level=${level}, tasks[0]=0x${taskPtr.toString(16)}`);
                }
            }
        }
    }
}

console.log(`\nFound ${foundPids} potential struct pid instances`);

fs.closeSync(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Analysis:');
console.log('---------');
console.log('The struct pid is the KEY to process enumeration!');
console.log('Each process has a struct pid that links to its task_struct.');
console.log('The kernel maintains these in various data structures.');
console.log('');
console.log('To achieve 100% discovery, we should:');
console.log('1. Find all struct pid instances');
console.log('2. Follow their task pointers to get task_structs');
console.log('3. This avoids the page-straddling problem!');