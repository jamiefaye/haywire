#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_OFFSET = 0x438;
const SLAB_SIZE = 0x8000;
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== Analyzing Task List Connectivity ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const fileSize = fs.fstatSync(fd).size;

function paToVA(pa) {
    return BigInt(pa) + KERNEL_VA_OFFSET;
}

function vaToPA(va) {
    return Number(va - KERNEL_VA_OFFSET);
}

function readTaskStruct(fd, slabPA, slabOffset) {
    const taskPA = slabPA + slabOffset;
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    const offset = taskPA - GUEST_RAM_START;
    
    if (offset < 0 || offset + TASK_STRUCT_SIZE > fileSize) {
        return null;
    }
    
    try {
        fs.readSync(fd, taskData, 0, TASK_STRUCT_SIZE, offset);
    } catch (e) {
        return null;
    }
    
    const pid = taskData.readUint32LE(PID_OFFSET);
    if (pid < 1 || pid > 32768) return null;
    
    const commBuffer = taskData.subarray(COMM_OFFSET, COMM_OFFSET + 16);
    const comm = commBuffer.toString('ascii').split('\0')[0];
    
    if (!comm || comm.length === 0 || comm.length > 15) return null;
    if (!/^[\x20-\x7E]+$/.test(comm)) return null;
    
    // Get list pointers
    const nextPtr = taskData.readBigUint64LE(TASKS_OFFSET);
    const prevPtr = taskData.readBigUint64LE(TASKS_OFFSET + 8);
    
    return {
        pa: taskPA,
        va: paToVA(taskPA),
        pid,
        comm,
        next: nextPtr,
        prev: prevPtr
    };
}

// Scan and collect all valid tasks
console.log('Scanning for task_structs...');
const tasks = [];
const tasksByVA = new Map();

for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + fileSize; pa += SLAB_SIZE) {
    for (const offset of SLAB_OFFSETS) {
        const task = readTaskStruct(fd, pa, offset);
        if (task) {
            tasks.push(task);
            // Store by the VA of the tasks field (what pointers point to)
            const tasksFieldVA = task.va + BigInt(TASKS_OFFSET);
            tasksByVA.set(tasksFieldVA.toString(), task);
        }
    }
}

console.log(`Found ${tasks.length} valid task_structs\n`);

// Analyze connectivity
console.log('Analyzing list connectivity...');
console.log('-'.repeat(70));

let connectedCount = 0;
let brokenNextCount = 0;
let brokenPrevCount = 0;
let selfLinkedCount = 0;

for (const task of tasks) {
    const tasksFieldVA = task.va + BigInt(TASKS_OFFSET);
    
    // Check if this task's pointers are valid
    const nextValid = tasksByVA.has(task.next.toString());
    const prevValid = tasksByVA.has(task.prev.toString());
    
    // Check for self-linking
    if (task.next === tasksFieldVA && task.prev === tasksFieldVA) {
        selfLinkedCount++;
        if (selfLinkedCount <= 3) {
            console.log(`Self-linked: PID ${task.pid} "${task.comm}"`);
            console.log(`  VA: 0x${task.va.toString(16)}`);
        }
    } else if (nextValid && prevValid) {
        connectedCount++;
        if (connectedCount <= 3) {
            console.log(`Connected: PID ${task.pid} "${task.comm}"`);
            const nextTask = tasksByVA.get(task.next.toString());
            const prevTask = tasksByVA.get(task.prev.toString());
            console.log(`  Next -> PID ${nextTask.pid} "${nextTask.comm}"`);
            console.log(`  Prev -> PID ${prevTask.pid} "${prevTask.comm}"`);
        }
    } else {
        if (!nextValid) brokenNextCount++;
        if (!prevValid) brokenPrevCount++;
        
        if (brokenNextCount + brokenPrevCount <= 5) {
            console.log(`Broken links: PID ${task.pid} "${task.comm}"`);
            console.log(`  Next: 0x${task.next.toString(16)} ${nextValid ? '✓' : '✗'}`);
            console.log(`  Prev: 0x${task.prev.toString(16)} ${prevValid ? '✓' : '✗'}`);
        }
    }
}

console.log('\n' + '='.repeat(70) + '\n');
console.log('Summary:');
console.log(`  Total tasks: ${tasks.length}`);
console.log(`  Self-linked: ${selfLinkedCount}`);
console.log(`  Fully connected: ${connectedCount}`);
console.log(`  Broken next: ${brokenNextCount}`);
console.log(`  Broken prev: ${brokenPrevCount}`);

// Try to follow chains
console.log('\n' + '='.repeat(70) + '\n');
console.log('Attempting to follow task chains...');

const visited = new Set();
const chains = [];

for (const task of tasks) {
    const tasksFieldVA = task.va + BigInt(TASKS_OFFSET);
    
    if (visited.has(tasksFieldVA.toString())) continue;
    
    // Try to follow this chain
    const chain = [];
    let current = tasksFieldVA;
    let maxSteps = 50;
    
    while (maxSteps-- > 0) {
        if (visited.has(current.toString())) break;
        visited.add(current.toString());
        
        const currentTask = tasksByVA.get(current.toString());
        if (!currentTask) break;
        
        chain.push(currentTask);
        
        // Move to next
        current = currentTask.next;
        
        // Check if we've come full circle
        if (current === tasksFieldVA) {
            chain.push({ circular: true });
            break;
        }
    }
    
    if (chain.length > 1) {
        chains.push(chain);
    }
}

console.log(`Found ${chains.length} task chains:`);
for (let i = 0; i < Math.min(3, chains.length); i++) {
    const chain = chains[i];
    const circular = chain[chain.length - 1].circular;
    const length = circular ? chain.length - 1 : chain.length;
    
    console.log(`\nChain ${i + 1}: ${length} tasks${circular ? ' (circular)' : ''}`);
    for (let j = 0; j < Math.min(5, length); j++) {
        const t = chain[j];
        console.log(`  ${j}: PID ${t.pid} "${t.comm}"`);
    }
    if (length > 5) {
        console.log(`  ... and ${length - 5} more`);
    }
}

fs.closeSync(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Key Insights:');
console.log('------------');
if (selfLinkedCount === tasks.length) {
    console.log('ALL tasks are self-linked!');
    console.log('This suggests we\'re looking at a memory snapshot where');
    console.log('the task list pointers haven\'t been initialized yet.');
    console.log('');
    console.log('This explains why following the list doesn\'t work!');
    console.log('We need to use a different enumeration method.');
} else if (connectedCount > tasks.length * 0.8) {
    console.log('Most tasks are properly connected in a list.');
    console.log('We can follow the list for enumeration.');
} else {
    console.log('Task list connectivity is mixed.');
    console.log('Some tasks are connected, others are isolated.');
}