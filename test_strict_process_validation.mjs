#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const MM_OFFSET = 0x6d0;
const TASKS_LIST_OFFSET = 0x7e0;
const REAL_PARENT_OFFSET = 0x7d0;
const PARENT_OFFSET = 0x7d8;

// SLAB offsets we check
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];
const PAGE_STRADDLE_OFFSETS = [0x0, 0x380, 0x700];

console.log('=== Strict Process Validation Test ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const stats = fs.fstatSync(fd);
const fileSize = stats.size;

console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB\n`);

// Load ground truth for known process names
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
console.log(`Loaded ${groundTruth.length} ground truth processes\n`);
console.log(`Known commands: ${Array.from(knownCommands).slice(0, 10).join(', ')}...\n`);

// Track findings
const foundProcesses = [];
const falsePositives = [];
const offsetHistogram = new Map();

function isValidKernelPointer(value) {
    // Kernel pointers should be in the 0xffff... range
    return value >= 0xffff000000000000n && value < 0xffffffffffffffffn;
}

function isValidUserPointer(value) {
    // User pointers should be < 0x0000800000000000 (user space limit)
    return value > 0 && value < 0x0000800000000000n;
}

function validateTaskStruct(fd, taskOffset, offset) {
    try {
        // Read PID
        const pidBuffer = Buffer.allocUnsafe(4);
        fs.readSync(fd, pidBuffer, 0, 4, taskOffset + PID_OFFSET);
        const pid = pidBuffer.readUint32LE(0);
        
        // Basic PID validation
        if (pid < 0 || pid > 32768) return null;
        
        // Read comm
        const commBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, commBuffer, 0, 16, taskOffset + COMM_OFFSET);
        const comm = commBuffer.toString('ascii').split('\0')[0];
        
        // Basic comm validation
        if (!comm || comm.length === 0 || comm.length > 15) return null;
        if (!/^[\x20-\x7E]+$/.test(comm)) return null;
        
        // Read additional fields for stronger validation
        const structBuffer = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
        fs.readSync(fd, structBuffer, 0, TASK_STRUCT_SIZE, taskOffset);
        
        // Read linked list pointers
        const tasksNext = structBuffer.readBigUint64LE(TASKS_LIST_OFFSET);
        const tasksPrev = structBuffer.readBigUint64LE(TASKS_LIST_OFFSET + 8);
        
        // Read parent pointers
        const realParent = structBuffer.readBigUint64LE(REAL_PARENT_OFFSET);
        const parent = structBuffer.readBigUint64LE(PARENT_OFFSET);
        
        // Read mm pointer
        const mmPointer = structBuffer.readBigUint64LE(MM_OFFSET);
        
        // Calculate validation score
        let score = 0;
        let reasons = [];
        
        // 1. Check if comm matches known process names
        if (knownCommands.has(comm)) {
            score += 3;
            reasons.push('known_comm');
        }
        
        // 2. Check linked list pointers
        if (isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev)) {
            score += 2;
            reasons.push('valid_list_ptrs');
        }
        
        // 3. Check parent pointers
        if (isValidKernelPointer(realParent) && isValidKernelPointer(parent)) {
            score += 2;
            reasons.push('valid_parent_ptrs');
        }
        
        // 4. Check mm pointer (can be 0 for kernel threads)
        if (mmPointer === 0n || isValidKernelPointer(mmPointer)) {
            score += 1;
            reasons.push('valid_mm');
        }
        
        // 5. PID 1 should be systemd or init
        if (pid === 1 && (comm === 'systemd' || comm === 'init')) {
            score += 5;
            reasons.push('init_process');
        }
        
        // 6. Check if PID matches known ground truth
        const groundTruthMatch = groundTruth.find(p => p.pid === pid);
        if (groundTruthMatch) {
            if (groundTruthMatch.comm === comm) {
                score += 5;
                reasons.push('exact_match');
            } else if (groundTruthMatch.comm === 'unknown' || comm === 'unknown') {
                score += 2;
                reasons.push('pid_match');
            }
        }
        
        return {
            pid,
            comm,
            taskOffset,
            offset,
            pa: taskOffset + GUEST_RAM_START,
            score,
            reasons,
            tasksNext: `0x${tasksNext.toString(16)}`,
            tasksPrev: `0x${tasksPrev.toString(16)}`,
            mmPointer: `0x${mmPointer.toString(16)}`,
            realParent: `0x${realParent.toString(16)}`,
            parent: `0x${parent.toString(16)}`
        };
        
    } catch (e) {
        return null;
    }
}

console.log('Scanning for processes with strict validation...\n');

// Scan first 2GB
const scanSize = Math.min(2 * 1024 * 1024 * 1024, fileSize);
let pagesScanned = 0;

for (let pageStart = 0; pageStart < scanSize; pageStart += PAGE_SIZE) {
    pagesScanned++;
    
    if (pageStart % (100 * 1024 * 1024) === 0) {
        process.stdout.write(`  Scanning ${(pageStart / (1024 * 1024)).toFixed(0)}MB...\r`);
    }
    
    // Check all our offsets
    const offsetsToCheck = [...SLAB_OFFSETS, ...PAGE_STRADDLE_OFFSETS];
    
    for (const offset of offsetsToCheck) {
        const taskOffset = pageStart + offset;
        
        if (taskOffset + TASK_STRUCT_SIZE > fileSize) continue;
        
        const result = validateTaskStruct(fd, taskOffset, offset);
        if (result) {
            if (result.score >= 3) {  // Require minimum score
                foundProcesses.push(result);
                
                const key = `0x${offset.toString(16)}`;
                offsetHistogram.set(key, (offsetHistogram.get(key) || 0) + 1);
            } else {
                falsePositives.push(result);
            }
        }
    }
}

console.log(`\n\nFound ${foundProcesses.length} likely processes`);
console.log(`Found ${falsePositives.length} false positives (low score)\n`);

// Sort by score
foundProcesses.sort((a, b) => b.score - a.score);

// Show high-confidence matches
console.log('=== HIGH CONFIDENCE MATCHES (score >= 5) ===');
const highConfidence = foundProcesses.filter(p => p.score >= 5);
console.log(`Count: ${highConfidence.length}\n`);

for (const p of highConfidence.slice(0, 20)) {
    console.log(`PID ${p.pid}: ${p.comm}`);
    console.log(`  Score: ${p.score} (${p.reasons.join(', ')})`);
    console.log(`  PA: 0x${p.pa.toString(16)}, offset: 0x${p.offset.toString(16)}`);
    console.log(`  tasks: next=${p.tasksNext}, prev=${p.tasksPrev}`);
    console.log(`  mm: ${p.mmPointer}`);
    console.log('');
}

// Show medium confidence
console.log('=== MEDIUM CONFIDENCE (score 3-4) ===');
const mediumConfidence = foundProcesses.filter(p => p.score >= 3 && p.score < 5);
console.log(`Count: ${mediumConfidence.length}\n`);

for (const p of mediumConfidence.slice(0, 10)) {
    console.log(`PID ${p.pid}: ${p.comm} - Score: ${p.score} (${p.reasons.join(', ')})`);
}

// Analyze offset distribution
console.log('\n=== OFFSET DISTRIBUTION ===');
for (const [offset, count] of offsetHistogram.entries()) {
    const pct = ((count / foundProcesses.length) * 100).toFixed(1);
    console.log(`${offset}: ${count} (${pct}%)`);
}

// Compare with ground truth
console.log('\n=== COMPARISON WITH GROUND TRUTH ===');
console.log(`Ground truth: ${groundTruth.length} processes`);
console.log(`High confidence: ${highConfidence.length} processes`);
console.log(`Total found: ${foundProcesses.length} processes`);

// Find exact matches
const exactMatches = foundProcesses.filter(p => p.reasons.includes('exact_match'));
console.log(`\nExact matches: ${exactMatches.length}`);

// Find which ground truth processes we missed
const foundPids = new Set(highConfidence.map(p => p.pid));
const missedProcesses = groundTruth.filter(gt => !foundPids.has(gt.pid));
console.log(`\nMissed ${missedProcesses.length} processes:`);
for (const p of missedProcesses.slice(0, 20)) {
    console.log(`  PID ${p.pid}: ${p.comm}`);
}

// Sample of false positives
if (falsePositives.length > 0) {
    console.log('\n=== SAMPLE FALSE POSITIVES ===');
    for (const p of falsePositives.slice(0, 5)) {
        console.log(`PID ${p.pid}: "${p.comm}" - Score: ${p.score}`);
        console.log(`  Pointers: tasks=${p.tasksNext}, mm=${p.mmPointer}`);
    }
}

fs.closeSync(fd);