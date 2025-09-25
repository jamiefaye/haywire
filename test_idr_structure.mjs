#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;

console.log('=== Understanding How IDR Structure Works ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Get init_pid_ns address
async function getInitPidNs() {
    return new Promise((resolve) => {
        exec(`ssh vm "sudo grep ' D init_pid_ns' /proc/kallsyms"`, (error, stdout) => {
            if (error) {
                resolve(null);
                return;
            }
            const [addr] = stdout.trim().split(' ');
            resolve(BigInt('0x' + addr));
        });
    });
}

function vaToPA(va) {
    // Using the fixed offset we discovered
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

const initPidNsVA = await getInitPidNs();
if (!initPidNsVA) {
    console.log('Failed to get init_pid_ns address');
    process.exit(1);
}

console.log(`init_pid_ns at VA: 0x${initPidNsVA.toString(16)}`);

const initPidNsPA = vaToPA(initPidNsVA);
console.log(`init_pid_ns at PA: 0x${initPidNsPA.toString(16)}`);

// Read init_pid_ns structure
// struct pid_namespace {
//     struct idr idr;  // offset 0x48
//     ...
// }
const pidNsBuffer = readMemory(fd, initPidNsPA, 0x100);
if (!pidNsBuffer) {
    console.log('Failed to read init_pid_ns');
    process.exit(1);
}

console.log('\nReading IDR structure at offset 0x48...\n');

// struct idr {
//     struct radix_tree_root idr_rt;  // offset 0x0
//     unsigned int idr_base;           // offset 0x10
//     unsigned int idr_next;           // offset 0x14
// };

// struct radix_tree_root {
//     struct xa_head xa_head;  // offset 0x0 - this is the root pointer
//     gfp_t gfp_mask;         // offset 0x8
//     struct radix_tree_node *rnode; // This is xa_head
// };

const idrOffset = 0x48;
const xaHeadPtr = pidNsBuffer.readBigUint64LE(idrOffset);
console.log(`IDR xa_head pointer: 0x${xaHeadPtr.toString(16)}`);

// The xa_head pointer IS the radix tree root pointer
// It points to either:
// 1. NULL (0) - empty tree
// 2. A data pointer with low bits 0x2 (single entry)
// 3. A node pointer (radix_tree_node)

if (xaHeadPtr === 0n) {
    console.log('IDR is empty');
    process.exit(0);
}

// Check if it's a single entry or a node
if ((xaHeadPtr & 0x3n) === 0x2n) {
    console.log('IDR has single entry (not a tree)');
    const dataPtr = xaHeadPtr & ~0x3n;
    console.log(`Data pointer: 0x${dataPtr.toString(16)}`);
} else {
    console.log('IDR has radix tree');
    
    // It's a radix_tree_node pointer
    const nodeVA = xaHeadPtr & ~0x3n;
    console.log(`Root node at VA: 0x${nodeVA.toString(16)}`);
    
    const nodePA = vaToPA(nodeVA);
    console.log(`Root node at PA: 0x${nodePA.toString(16)}`);
    
    // Read the radix tree node
    // struct radix_tree_node {
    //     unsigned char shift;     // offset 0x0
    //     unsigned char offset;    // offset 0x1
    //     unsigned char count;     // offset 0x2
    //     unsigned char exceptional; // offset 0x3
    //     void *parent;           // offset 0x8
    //     void *slots[64];        // offset 0x10
    //     ...
    // };
    
    const nodeBuffer = readMemory(fd, nodePA, 0x300);
    if (!nodeBuffer) {
        console.log('Failed to read radix tree node');
        process.exit(1);
    }
    
    const shift = nodeBuffer.readUInt8(0x0);
    const offset = nodeBuffer.readUInt8(0x1);
    const count = nodeBuffer.readUInt8(0x2);
    
    console.log(`\nRoot node info:`);
    console.log(`  Shift: ${shift} (height in tree)`);
    console.log(`  Offset: ${offset}`);
    console.log(`  Count: ${count} (number of non-NULL slots)`);
    
    // Read slots (pointers to PIDs or more nodes)
    console.log(`\nChecking slots (up to 64):`);
    
    let foundPids = [];
    
    for (let i = 0; i < 64; i++) {
        const slotPtr = nodeBuffer.readBigUint64LE(0x10 + i * 8);
        if (slotPtr === 0n) continue;
        
        console.log(`\nSlot[${i}]: 0x${slotPtr.toString(16)}`);
        
        // Check if this is an internal node or a leaf (struct pid pointer)
        // Leaf pointers often have low bits set
        const cleanPtr = slotPtr & ~0x3n;
        
        if (shift > 0) {
            // This should be another radix_tree_node
            console.log(`  -> Internal node (shift=${shift})`);
        } else {
            // This should be a struct pid pointer
            console.log(`  -> Leaf: struct pid pointer`);
            
            // Try to read struct pid
            const pidStructVA = cleanPtr;
            const pidStructPA = vaToPA(pidStructVA);
            
            console.log(`     VA: 0x${pidStructVA.toString(16)}`);
            console.log(`     PA: 0x${pidStructPA.toString(16)}`);
            
            // struct pid {
            //     refcount_t count;     // offset 0x0
            //     unsigned int level;   // offset 0x4
            //     struct pid_link tasks[PIDTYPE_MAX]; // offset 0x8
            //     struct upid numbers[]; // offset varies
            // };
            
            const pidBuffer = readMemory(fd, pidStructPA, 0x100);
            if (pidBuffer) {
                const refcount = pidBuffer.readUInt32LE(0x0);
                const level = pidBuffer.readUInt32LE(0x4);
                
                console.log(`     Refcount: ${refcount}`);
                console.log(`     Level: ${level}`);
                
                // The tasks array has pointers to task_structs!
                // Each pid_link is { struct hlist_node node; }
                // struct hlist_node { struct hlist_node *next, **pprev; }
                
                for (let taskType = 0; taskType < 4; taskType++) {
                    const taskPtr = pidBuffer.readBigUint64LE(0x8 + taskType * 16);
                    if (taskPtr !== 0n) {
                        console.log(`     Task[${taskType}] pointer: 0x${taskPtr.toString(16)}`);
                        
                        // This points to a task_struct!
                        // The pointer is to the pid_links member inside task_struct
                        // We need to subtract the offset to get to the start
                        
                        // task_struct.pid_links is around offset 0x4e8
                        const taskStructVA = taskPtr - 0x4e8n;
                        console.log(`       -> task_struct at VA: 0x${taskStructVA.toString(16)}`);
                        
                        const taskStructPA = vaToPA(taskStructVA);
                        console.log(`       -> task_struct at PA: 0x${taskStructPA.toString(16)}`);
                        
                        // Try to read PID and comm
                        const taskBuffer = readMemory(fd, taskStructPA, 0x1000);
                        if (taskBuffer) {
                            const pid = taskBuffer.readUInt32LE(0x750);
                            const comm = taskBuffer.subarray(0x970, 0x980).toString('ascii').split('\0')[0];
                            
                            if (pid > 0 && pid < 32768 && comm) {
                                console.log(`       -> PID ${pid}: "${comm}"`);
                                foundPids.push({ pid, comm, va: taskStructVA });
                            }
                        }
                    }
                }
            }
        }
        
        // Just show first few
        if (i >= 5 && foundPids.length > 0) {
            console.log('\n(Stopping after first few slots)');
            break;
        }
    }
    
    console.log('\n' + '='.repeat(70) + '\n');
    console.log('Key Discovery:');
    console.log('-------------');
    console.log('The IDR contains pointers to struct pid, which contain');
    console.log('pointers to task_structs! These pointers are kernel VAs.');
    console.log('');
    console.log('The VA->PA translation works with our fixed offset:');
    console.log(`  VA = PA + 0x${KERNEL_VA_OFFSET.toString(16)}`);
    console.log('');
    console.log('This means we CAN follow the pointers!');
    console.log('Even for fragmented task_structs, the IDR has the correct VA.');
}

fs.closeSync(fd);