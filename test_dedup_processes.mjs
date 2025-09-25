#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

// SLAB offsets we check
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== Deduplicated Process Discovery ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const stats = fs.fstatSync(fd);
const fileSize = stats.size;

console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB\n`);

// Load ground truth
const groundTruth = fs.readFileSync('ground_truth_processes.txt', 'utf-8')
    .split('\n')
    .filter(line => line.trim())
    .map(line => {
        const parts = line.split(' ');
        const pid = parseInt(parts[0]);
        const comm = parts.slice(1).join(' ');
        return { pid, comm };
    });

const knownCommands = new Set(groundTruth.map(p => p.comm));

function isValidKernelPointer(value) {
    return value >= 0xffff000000000000n && value < 0xffffffffffffffffn;
}

function validateTaskStruct(fd, taskOffset) {
    try {
        // Read PID
        const pidBuffer = Buffer.allocUnsafe(4);
        fs.readSync(fd, pidBuffer, 0, 4, taskOffset + PID_OFFSET);
        const pid = pidBuffer.readUint32LE(0);
        
        if (pid < 0 || pid > 32768) return null;
        
        // Read comm
        const commBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, commBuffer, 0, 16, taskOffset + COMM_OFFSET);
        const comm = commBuffer.toString('ascii').split('\0')[0];
        
        if (!comm || comm.length === 0 || comm.length > 15) return null;
        if (!/^[\x20-\x7E]+$/.test(comm)) return null;
        
        // Read linked list pointers
        const tasksBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, tasksBuffer, 0, 16, taskOffset + TASKS_LIST_OFFSET);
        const tasksNext = tasksBuffer.readBigUint64LE(0);
        const tasksPrev = tasksBuffer.readBigUint64LE(8);
        
        // Score based on validation
        let score = 0;
        if (knownCommands.has(comm)) score += 3;
        if (isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev)) score += 2;
        
        const groundTruthMatch = groundTruth.find(p => p.pid === pid && p.comm === comm);
        if (groundTruthMatch) score += 5;
        
        if (score < 3) return null;
        
        return {
            pid,
            comm,
            taskOffset,
            pa: taskOffset + GUEST_RAM_START,
            score,
            tasksNext: `0x${tasksNext.toString(16)}`,
            tasksPrev: `0x${tasksPrev.toString(16)}`
        };
        
    } catch (e) {
        return null;
    }
}

console.log('Scanning with SLAB offsets only...\n');

// Map to deduplicate by PID
const processesByPid = new Map();
const offsetStats = new Map();

// Scan first 2GB
const scanSize = Math.min(2 * 1024 * 1024 * 1024, fileSize);

for (let pageStart = 0; pageStart < scanSize; pageStart += PAGE_SIZE) {
    if (pageStart % (100 * 1024 * 1024) === 0) {
        process.stdout.write(`  Scanning ${(pageStart / (1024 * 1024)).toFixed(0)}MB...\r`);
    }
    
    // Only check the 3 SLAB offsets
    for (const offset of SLAB_OFFSETS) {
        const taskOffset = pageStart + offset;
        
        if (taskOffset + TASK_STRUCT_SIZE > fileSize) continue;
        
        const result = validateTaskStruct(fd, taskOffset);
        if (result) {
            // Track offset statistics
            const key = `0x${offset.toString(16)}`;
            offsetStats.set(key, (offsetStats.get(key) || 0) + 1);
            
            // Deduplicate by PID - keep highest scoring
            if (!processesByPid.has(result.pid) || processesByPid.get(result.pid).score < result.score) {
                processesByPid.set(result.pid, { ...result, offset });
            }
        }
    }
}

console.log('\n\n=== RESULTS ===\n');

const uniqueProcesses = Array.from(processesByPid.values());
const highConfidence = uniqueProcesses.filter(p => p.score >= 5);

console.log(`Total unique processes: ${uniqueProcesses.length}`);
console.log(`High confidence: ${highConfidence.length}`);
console.log(`Ground truth: ${groundTruth.length}\n`);

// Offset distribution before deduplication
console.log('=== OFFSET DISTRIBUTION (before dedup) ===');
for (const [offset, count] of offsetStats.entries()) {
    console.log(`${offset}: ${count} task_structs found`);
}

// Offset distribution after deduplication
console.log('\n=== OFFSET DISTRIBUTION (after dedup) ===');
const dedupOffsetStats = new Map();
for (const p of uniqueProcesses) {
    const key = `0x${p.offset.toString(16)}`;
    dedupOffsetStats.set(key, (dedupOffsetStats.get(key) || 0) + 1);
}

for (const [offset, count] of dedupOffsetStats.entries()) {
    const pct = ((count / uniqueProcesses.length) * 100).toFixed(1);
    console.log(`${offset}: ${count} processes (${pct}%)`);
}

// Analysis of SLAB structure at offset 0x2380
console.log('\n=== SLAB STRUCTURE ANALYSIS ===');
console.log('If task_structs are 0x2380 bytes and stored in SLABs:');
console.log('  Offset 0x0000: First task_struct in SLAB');
console.log('  Offset 0x2380: Second task_struct in SLAB');
console.log('  Offset 0x4700: Third task_struct in SLAB');
console.log('');
console.log('The 0x4700 offset straddles pages:');
console.log('  Starts at page offset 0x700 (in page 1)');
console.log('  Ends at page offset 0xa80 (in page 2)');
console.log('  PID field at 0x4700 + 0x750 = 0x4e50 (page 1)');
console.log('  comm field at 0x4700 + 0x970 = 0x5070 (page 1)');
console.log('');

// Show what we're missing
const foundPids = new Set(highConfidence.map(p => p.pid));
const missedProcesses = groundTruth.filter(gt => !foundPids.has(gt.pid));

console.log(`=== MISSING PROCESSES ===`);
console.log(`Missing ${missedProcesses.length} of ${groundTruth.length} processes\n`);

// Group missed by type
const kernelThreads = missedProcesses.filter(p => 
    p.comm.startsWith('kworker') || 
    p.comm.startsWith('ksoftirqd') ||
    p.comm.startsWith('migration') ||
    p.comm.startsWith('rcu_') ||
    p.comm.includes('kthread') ||
    p.comm.startsWith('cpuhp') ||
    p.comm.startsWith('idle_inject') ||
    p.pid < 100
);

const userProcesses = missedProcesses.filter(p => !kernelThreads.includes(p));

console.log(`Missed kernel threads: ${kernelThreads.length}`);
console.log(`Missed user processes: ${userProcesses.length}\n`);

if (userProcesses.length > 0) {
    console.log('Missed user processes:');
    for (const p of userProcesses.slice(0, 20)) {
        console.log(`  PID ${p.pid}: ${p.comm}`);
    }
}

fs.closeSync(fd);