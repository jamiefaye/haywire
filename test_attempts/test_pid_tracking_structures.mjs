#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;

console.log('=== Finding Where Kernel Tracks PIDs ===\n');

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

// Get kernel symbols
async function getKernelSymbols() {
    return new Promise((resolve) => {
        const symbols = [
            'init_task',
            'init_pid_ns', 
            'pid_hash',
            'tasklist_lock',
            'init_struct_pid'
        ];
        
        const pattern = symbols.join('\|');
        exec(`ssh vm "sudo grep -E '${pattern}' /proc/kallsyms"`, (error, stdout) => {
            if (error) {
                resolve({});
                return;
            }
            
            const result = {};
            for (const line of stdout.trim().split('\n')) {
                const [addr, type, name] = line.split(' ');
                result[name] = BigInt('0x' + addr);
            }
            resolve(result);
        });
    });
}

const symbols = await getKernelSymbols();

console.log('Kernel Symbols:');
for (const [name, va] of Object.entries(symbols)) {
    console.log(`  ${name}: VA 0x${va.toString(16)}`);
}

console.log('\n' + '='.repeat(70) + '\n');

// Check init_task
if (symbols.init_task) {
    console.log('Checking init_task structure...');
    const initTaskPA = vaToPA(symbols.init_task);
    const taskBuffer = readMemory(fd, initTaskPA, 0x1000);
    
    if (taskBuffer) {
        // Check PID and comm
        const pid = taskBuffer.readUint32LE(0x750);
        const comm = taskBuffer.subarray(0x970, 0x980).toString('ascii').split('\0')[0];
        console.log(`  PID: ${pid}, comm: "${comm}"`);
        
        // Check tasks list pointers
        const nextPtr = taskBuffer.readBigUint64LE(0x438);
        const prevPtr = taskBuffer.readBigUint64LE(0x440);
        console.log(`  tasks.next: 0x${nextPtr.toString(16)}`);
        console.log(`  tasks.prev: 0x${prevPtr.toString(16)}`);
        
        // The pointers point to the tasks field in other task_structs
        // To get to the start of task_struct, subtract 0x438
        if (nextPtr !== 0n && nextPtr > 0xffff000000000000n) {
            const nextTaskVA = nextPtr - 0x438n;
            console.log(`  Next task_struct at VA: 0x${nextTaskVA.toString(16)}`);
            
            const nextTaskPA = vaToPA(nextTaskVA);
            const nextBuffer = readMemory(fd, nextTaskPA, 0x1000);
            if (nextBuffer) {
                const nextPid = nextBuffer.readUint32LE(0x750);
                const nextComm = nextBuffer.subarray(0x970, 0x980).toString('ascii').split('\0')[0];
                console.log(`    -> PID ${nextPid}: "${nextComm}"`);
            }
        }
    }
}

console.log('\n' + '='.repeat(70) + '\n');

// Check init_pid_ns again with correct understanding
if (symbols.init_pid_ns) {
    console.log('Checking init_pid_ns (PID namespace)...');
    const pidNsPA = vaToPA(symbols.init_pid_ns);
    const pidNsBuffer = readMemory(fd, pidNsPA, 0x200);
    
    if (pidNsBuffer) {
        // struct pid_namespace has idr at offset 0x48
        console.log('  IDR structure at offset 0x48:');
        const idrRoot = pidNsBuffer.readBigUint64LE(0x48);
        console.log(`    xa_head: 0x${idrRoot.toString(16)}`);
        
        // Check pid_hash instead
        console.log('\n  Alternative: checking other offsets...');
        for (let offset = 0; offset < 0x100; offset += 8) {
            const ptr = pidNsBuffer.readBigUint64LE(offset);
            if (ptr > 0xffff000000000000n && ptr !== 0xffffffffffffffffn) {
                console.log(`    Offset 0x${offset.toString(16)}: 0x${ptr.toString(16)}`);
            }
        }
    }
}

console.log('\n' + '='.repeat(70) + '\n');

// Check pid_hash if available
if (symbols.pid_hash) {
    console.log('Checking pid_hash (global PID hash table)...');
    const pidHashPA = vaToPA(symbols.pid_hash);
    const hashBuffer = readMemory(fd, pidHashPA, 0x100);
    
    if (hashBuffer) {
        // pid_hash is an array of pointers to hash lists
        console.log('  First few hash buckets:');
        for (let i = 0; i < 8; i++) {
            const ptr = hashBuffer.readBigUint64LE(i * 8);
            if (ptr !== 0n) {
                console.log(`    Bucket[${i}]: 0x${ptr.toString(16)}`);
                
                // This should point to a struct pid
                if (ptr > 0xffff000000000000n) {
                    const pidStructPA = vaToPA(ptr);
                    const pidBuffer = readMemory(fd, pidStructPA, 0x100);
                    if (pidBuffer) {
                        const refcount = pidBuffer.readUint32LE(0x0);
                        const level = pidBuffer.readUint32LE(0x4);
                        console.log(`      -> struct pid: refcount=${refcount}, level=${level}`);
                    }
                }
            }
        }
    }
}

console.log('\n' + '='.repeat(70) + '\n');

// Try following init_task list more carefully
console.log('Following init_task list with proper pointer adjustment...');
if (symbols.init_task) {
    const visited = new Set();
    let current = symbols.init_task;
    let count = 0;
    const maxCount = 10;
    
    while (count < maxCount) {
        if (visited.has(current.toString())) {
            console.log('Circular list detected');
            break;
        }
        visited.add(current.toString());
        
        const taskPA = vaToPA(current);
        const taskBuffer = readMemory(fd, taskPA, 0x1000);
        
        if (!taskBuffer) {
            console.log(`Failed to read task at VA 0x${current.toString(16)}`);
            break;
        }
        
        const pid = taskBuffer.readUint32LE(0x750);
        const comm = taskBuffer.subarray(0x970, 0x980).toString('ascii').split('\0')[0];
        console.log(`Task ${count}: PID ${pid} "${comm}" at VA 0x${current.toString(16)}`);
        
        // Get next pointer (points to tasks field in next task_struct)
        const nextTasksPtr = taskBuffer.readBigUint64LE(0x438);
        
        // Check if we're back at init_task
        if (nextTasksPtr === symbols.init_task + 0x438n) {
            console.log('Completed circular list');
            break;
        }
        
        // Calculate next task_struct VA (subtract offset to get to start)
        current = nextTasksPtr - 0x438n;
        count++;
    }
    
    console.log(`\nFound ${count} tasks in init_task list`);
}

fs.closeSync(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Key Findings:');
console.log('------------');
console.log('1. init_task list can be followed if pages are contiguous');
console.log('2. IDR in init_pid_ns appears empty (might be using different structure)');
console.log('3. pid_hash might be a better approach for finding all PIDs');
console.log('4. We need to handle page straddling for complete enumeration');