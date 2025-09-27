#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const SLAB_SIZE = 0x8000;
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== Why Are We Missing Processes? ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const fileSize = fs.fstatSync(fd).size;

// Get ground truth PIDs
async function getGroundTruth() {
    return new Promise((resolve) => {
        exec(`ssh vm "ps aux"`, (error, stdout) => {
            if (error) {
                resolve({});
                return;
            }
            
            const processes = {};
            const lines = stdout.trim().split('\n').slice(1); // Skip header
            for (const line of lines) {
                const parts = line.trim().split(/\s+/);
                const pid = parseInt(parts[1]);
                const comm = parts[10];
                if (pid > 0 && pid <= 32768) {
                    processes[pid] = comm;
                }
            }
            resolve(processes);
        });
    });
}

function readTaskStruct(fd, pa, offset) {
    const taskPA = pa + offset;
    const buffer = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    const fileOffset = taskPA - GUEST_RAM_START;
    
    if (fileOffset < 0 || fileOffset + TASK_STRUCT_SIZE > fileSize) {
        return null;
    }
    
    try {
        const bytesRead = fs.readSync(fd, buffer, 0, TASK_STRUCT_SIZE, fileOffset);
        if (bytesRead < TASK_STRUCT_SIZE) {
            return null;
        }
    } catch (e) {
        return null;
    }
    
    const pid = buffer.readUInt32LE(PID_OFFSET);
    const comm = buffer.subarray(COMM_OFFSET, COMM_OFFSET + 16).toString('ascii').split('\0')[0];
    
    return { pid, comm, pa: taskPA };
}

const groundTruth = await getGroundTruth();
console.log(`Ground truth: ${Object.keys(groundTruth).length} processes from ps aux\n`);

// Scan for processes
const found = {};
const invalidReasons = {
    'invalid_pid': [],
    'invalid_comm': [],
    'duplicate': []
};

for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + fileSize; pa += SLAB_SIZE) {
    for (const offset of SLAB_OFFSETS) {
        const task = readTaskStruct(fd, pa, offset);
        if (!task) continue;
        
        // Check various validation criteria
        if (task.pid < 1 || task.pid > 32768) {
            invalidReasons.invalid_pid.push(task);
            continue;
        }
        
        if (!task.comm || task.comm.length === 0 || task.comm.length > 15) {
            invalidReasons.invalid_comm.push(task);
            continue;
        }
        
        // Check for printable characters
        if (!/^[\x20-\x7E]+$/.test(task.comm)) {
            invalidReasons.invalid_comm.push(task);
            continue;
        }
        
        if (found[task.pid]) {
            invalidReasons.duplicate.push(task);
            continue;
        }
        
        found[task.pid] = task;
    }
}

console.log(`Found ${Object.keys(found).length} unique valid processes\n`);

// Analyze what we found vs ground truth
const foundPids = Object.keys(found).map(p => parseInt(p));
const truthPids = Object.keys(groundTruth).map(p => parseInt(p));

const matched = truthPids.filter(p => foundPids.includes(p));
const missed = truthPids.filter(p => !foundPids.includes(p));
const extra = foundPids.filter(p => !truthPids.includes(p));

console.log('Discovery Analysis:');
console.log('------------------');
console.log(`Matched: ${matched.length}/${truthPids.length} (${(matched.length * 100 / truthPids.length).toFixed(1)}%)`);
console.log(`Missed: ${missed.length}`);
console.log(`Extra (stale): ${extra.length}\n`);

if (missed.length > 0) {
    console.log('Sample of missed processes:');
    for (const pid of missed.slice(0, 10)) {
        console.log(`  PID ${pid}: ${groundTruth[pid]}`);
    }
}

console.log('\nInvalid task_structs filtered out:');
console.log(`  Invalid PID: ${invalidReasons.invalid_pid.length}`);
console.log(`  Invalid comm: ${invalidReasons.invalid_comm.length}`);
console.log(`  Duplicates: ${invalidReasons.duplicate.length}`);

// Check if missed processes might be in different memory regions
console.log('\n' + '='.repeat(70) + '\n');
console.log('Hypothesis Testing:');
console.log('------------------');

// Check offset distribution of found processes
const offsetCounts = { 0x0: 0, 0x2380: 0, 0x4700: 0 };
for (const task of Object.values(found)) {
    const offset = task.pa & 0x7fff;
    if (offset === 0x0) offsetCounts[0x0]++;
    else if (offset === 0x2380) offsetCounts[0x2380]++;
    else if (offset === 0x4700) offsetCounts[0x4700]++;
}

console.log('\nOffset distribution of FOUND processes:');
for (const [offset, count] of Object.entries(offsetCounts)) {
    const pct = (count * 100 / Object.keys(found).length).toFixed(1);
    console.log(`  Offset ${offset}: ${count} (${pct}%)`);
}

if (offsetCounts[0x4700] === 0) {
    console.log('\n🔥 KEY FINDING: Zero processes found at offset 0x4700!');
    console.log('This suggests ALL task_structs at 0x4700 are failing.');
    console.log('But it\'s NOT due to fragmentation (PTEs show contiguous).');
    console.log('\nPossible reasons:');
    console.log('1. Offset 0x4700 straddles a SLAB boundary');
    console.log('2. Different SLAB size or layout than expected');
    console.log('3. Alignment issue with our reading');
}

fs.closeSync(fd);