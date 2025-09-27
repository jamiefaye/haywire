#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

// SLAB offsets
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== Full Memory Scan for Processes ===\n');

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
const groundTruthByPid = new Map(groundTruth.map(p => [p.pid, p]));

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
        
        // Read linked list pointers for validation
        const tasksBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, tasksBuffer, 0, 16, taskOffset + TASKS_LIST_OFFSET);
        const tasksNext = tasksBuffer.readBigUint64LE(0);
        const tasksPrev = tasksBuffer.readBigUint64LE(8);
        
        // Calculate score
        let score = 0;
        if (knownCommands.has(comm)) score += 3;
        if (isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev)) score += 2;
        
        // Check ground truth match
        const gt = groundTruthByPid.get(pid);
        if (gt && gt.comm === comm) score += 5;
        
        if (score < 3) return null;
        
        return {
            pid,
            comm,
            taskOffset,
            pa: taskOffset + GUEST_RAM_START,
            score
        };
        
    } catch (e) {
        return null;
    }
}

// Map to deduplicate by PID
const processesByPid = new Map();
const regionStats = new Map();

// Scan entire 6GB file
console.log('Scanning entire memory file...\n');

for (let pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
    if (pageStart % (500 * 1024 * 1024) === 0) {
        const gb = (pageStart / (1024 * 1024 * 1024)).toFixed(2);
        process.stdout.write(`  Scanning at ${gb}GB...\r`);
    }
    
    // Determine region
    let region;
    const gbOffset = pageStart / (1024 * 1024 * 1024);
    if (gbOffset < 1) region = '0-1GB';
    else if (gbOffset < 2) region = '1-2GB';
    else if (gbOffset < 3) region = '2-3GB';
    else if (gbOffset < 4) region = '3-4GB';
    else if (gbOffset < 5) region = '4-5GB';
    else region = '5-6GB';
    
    for (const offset of SLAB_OFFSETS) {
        const taskOffset = pageStart + offset;
        
        if (taskOffset + TASK_STRUCT_SIZE > fileSize) continue;
        
        const result = validateTaskStruct(fd, taskOffset);
        if (result) {
            // Track region statistics
            if (!regionStats.has(region)) {
                regionStats.set(region, { count: 0, pids: [] });
            }
            const stats = regionStats.get(region);
            stats.count++;
            
            // Deduplicate by PID
            if (!processesByPid.has(result.pid) || processesByPid.get(result.pid).score < result.score) {
                processesByPid.set(result.pid, { ...result, offset, region });
                if (!stats.pids.includes(result.pid)) {
                    stats.pids.push(result.pid);
                }
            }
        }
    }
}

console.log('\n\n=== SCAN RESULTS ===\n');

const uniqueProcesses = Array.from(processesByPid.values());
const highConfidence = uniqueProcesses.filter(p => p.score >= 5);

console.log(`Total unique processes found: ${uniqueProcesses.length}`);
console.log(`High confidence: ${highConfidence.length}`);
console.log(`Ground truth: ${groundTruth.length}`);
console.log(`Discovery rate: ${((uniqueProcesses.length / groundTruth.length) * 100).toFixed(1)}%\n`);

// Show region distribution
console.log('=== PROCESSES BY MEMORY REGION ===');
for (const [region, stats] of regionStats.entries()) {
    console.log(`${region}: ${stats.pids.length} unique processes (${stats.count} total hits)`);
}

// Show which processes we found
console.log('\n=== FOUND PROCESSES BY TYPE ===');

const foundKernelThreads = uniqueProcesses.filter(p => 
    p.comm.startsWith('kworker') || 
    p.comm.startsWith('ksoftirqd') ||
    p.comm.startsWith('migration') ||
    p.comm.startsWith('rcu_') ||
    p.comm.includes('kthread') ||
    p.comm.startsWith('cpuhp') ||
    p.comm.startsWith('idle_inject') ||
    p.pid < 100
);

const foundUserProcesses = uniqueProcesses.filter(p => !foundKernelThreads.includes(p));

console.log(`Found kernel threads: ${foundKernelThreads.length}`);
console.log(`Found user processes: ${foundUserProcesses.length}`);

// Show what we're missing
const foundPids = new Set(uniqueProcesses.map(p => p.pid));
const missedProcesses = groundTruth.filter(gt => !foundPids.has(gt.pid));

console.log(`\n=== MISSING PROCESSES ===`);
console.log(`Missing ${missedProcesses.length} of ${groundTruth.length} processes\n`);

const missedKernelThreads = missedProcesses.filter(p => p.pid < 100 || p.comm.includes('kworker') || p.comm.includes('kthread'));
const missedUserProcesses = missedProcesses.filter(p => !missedKernelThreads.includes(p));

console.log(`Missing kernel threads: ${missedKernelThreads.length}`);
console.log(`Missing user processes: ${missedUserProcesses.length}`);

if (missedKernelThreads.length > 0) {
    console.log('\nSample missing kernel threads:');
    for (const p of missedKernelThreads.slice(0, 10)) {
        console.log(`  PID ${p.pid}: ${p.comm}`);
    }
}

if (missedUserProcesses.length > 0) {
    console.log('\nSample missing user processes:');
    for (const p of missedUserProcesses.slice(0, 10)) {
        console.log(`  PID ${p.pid}: ${p.comm}`);
    }
}

// Show where specific important processes were found
console.log('\n=== LOCATION OF KEY PROCESSES ===');
const keyPids = [1, 2, 254, 316, 901, 1177, 1546];
for (const pid of keyPids) {
    const proc = processesByPid.get(pid);
    if (proc) {
        console.log(`PID ${pid} (${proc.comm}): PA 0x${proc.pa.toString(16)} in ${proc.region}`);
    } else {
        const gt = groundTruthByPid.get(pid);
        if (gt) {
            console.log(`PID ${pid} (${gt.comm}): NOT FOUND`);
        }
    }
}

fs.closeSync(fd);