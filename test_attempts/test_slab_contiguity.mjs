#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== SLAB Contiguity Test ===\n');
console.log('Testing if SLAB pages are actually contiguous...\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Load ground truth for validation
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

// Test strategy: Find task_structs at different offsets and check if they're valid
// If SLAB pages aren't contiguous, we should see corruption when reading across boundaries

const testResults = {
    '0x0': { valid: 0, invalid: 0, examples: [] },
    '0x2380': { valid: 0, invalid: 0, examples: [] },
    '0x4700': { valid: 0, invalid: 0, examples: [] }
};

console.log('Scanning first 4GB for task_structs...\n');

const scanSize = Math.min(4 * 1024 * 1024 * 1024, fs.fstatSync(fd).size);

for (let pageStart = 0; pageStart < scanSize; pageStart += PAGE_SIZE) {
    if (pageStart % (500 * 1024 * 1024) === 0) {
        process.stdout.write(`  Scanning ${(pageStart / (1024 * 1024 * 1024)).toFixed(2)}GB...\r`);
    }
    
    for (const offset of [0x0, 0x2380, 0x4700]) {
        const taskOffset = pageStart + offset;
        
        if (taskOffset + TASK_STRUCT_SIZE > scanSize) continue;
        
        try {
            // Read the full struct as our scanner does
            const fullBuffer = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
            fs.readSync(fd, fullBuffer, 0, TASK_STRUCT_SIZE, taskOffset);
            
            // Extract fields
            const pid = fullBuffer.readUint32LE(PID_OFFSET);
            if (pid < 1 || pid > 32768) continue;
            
            const commBuffer = fullBuffer.slice(COMM_OFFSET, COMM_OFFSET + 16);
            const comm = commBuffer.toString('ascii').split('\0')[0];
            
            // Read linked list pointers
            const tasksNext = fullBuffer.readBigUint64LE(TASKS_LIST_OFFSET);
            const tasksPrev = fullBuffer.readBigUint64LE(TASKS_LIST_OFFSET + 8);
            
            // Now let's validate if this looks like a real task_struct
            const validation = {
                hasValidPid: pid > 0 && pid < 32768,
                hasValidComm: comm && comm.length > 0 && comm.length <= 15 && /^[\x20-\x7E]+$/.test(comm),
                hasKnownComm: knownCommands.has(comm),
                hasValidPointers: isValidKernelPointer(tasksNext) && isValidKernelPointer(tasksPrev),
                matchesGroundTruth: groundTruthByPid.has(pid) && groundTruthByPid.get(pid).comm === comm
            };
            
            // Calculate validity score
            let score = 0;
            if (validation.hasValidPid) score++;
            if (validation.hasValidComm) score++;
            if (validation.hasKnownComm) score += 2;
            if (validation.hasValidPointers) score += 2;
            if (validation.matchesGroundTruth) score += 3;
            
            const isValid = score >= 3;
            const offsetKey = `0x${offset.toString(16)}`;
            
            if (isValid) {
                testResults[offsetKey].valid++;
                if (testResults[offsetKey].examples.length < 3) {
                    testResults[offsetKey].examples.push({
                        pid, comm, 
                        pa: `0x${(taskOffset + GUEST_RAM_START).toString(16)}`,
                        score,
                        validation
                    });
                }
            } else if (validation.hasValidPid && validation.hasValidComm) {
                // Has basic fields but failed other validation
                testResults[offsetKey].invalid++;
                
                // Special check for offset 0x4700 - are we seeing corruption?
                if (offset === 0x4700 && testResults[offsetKey].invalid <= 5) {
                    console.log(`\nSuspicious at 0x4700 offset ${taskOffset.toString(16)}:`);
                    console.log(`  PID: ${pid}, comm: "${comm}"`);
                    console.log(`  Score: ${score}`);
                    console.log(`  Pointers valid: ${validation.hasValidPointers}`);
                    console.log(`  Known command: ${validation.hasKnownComm}`);
                    
                    // Check if we're reading garbage across non-contiguous pages
                    if (!validation.hasValidPointers && !validation.hasKnownComm) {
                        console.log(`  ⚠️  Possible non-contiguous pages!`);
                        
                        // Let's verify by reading pages separately
                        const page4Start = Math.floor(taskOffset / PAGE_SIZE) * PAGE_SIZE;
                        const page5Start = page4Start + PAGE_SIZE;
                        
                        // Read PID from page 4
                        const pidOffset = taskOffset + PID_OFFSET;
                        const pidBuffer2 = Buffer.allocUnsafe(4);
                        fs.readSync(fd, pidBuffer2, 0, 4, pidOffset);
                        const pid2 = pidBuffer2.readUint32LE(0);
                        
                        // Read comm from page 5
                        const commOffset = taskOffset + COMM_OFFSET;
                        const commBuffer2 = Buffer.allocUnsafe(16);
                        fs.readSync(fd, commBuffer2, 0, 16, commOffset);
                        const comm2 = commBuffer2.toString('ascii').split('\0')[0];
                        
                        console.log(`  Separate reads: PID=${pid2}, comm="${comm2}"`);
                        
                        if (pid !== pid2 || comm !== comm2) {
                            console.log(`  ❌ MISMATCH! Pages might not be contiguous!`);
                        }
                    }
                }
            }
            
        } catch (e) {
            // Read error - skip
        }
    }
}

console.log('\n\n=== RESULTS ===\n');

for (const [offset, results] of Object.entries(testResults)) {
    console.log(`Offset ${offset}:`);
    console.log(`  Valid task_structs: ${results.valid}`);
    console.log(`  Invalid (suspicious): ${results.invalid}`);
    
    const total = results.valid + results.invalid;
    if (total > 0) {
        const validPct = ((results.valid / total) * 100).toFixed(1);
        console.log(`  Validity rate: ${validPct}%`);
    }
    
    if (results.examples.length > 0) {
        console.log(`  Examples of valid:`);
        for (const ex of results.examples) {
            console.log(`    PID ${ex.pid}: ${ex.comm} at ${ex.pa} (score: ${ex.score})`);
        }
    }
    console.log('');
}

// Analysis
console.log('=== ANALYSIS ===\n');

const offset4700 = testResults['0x4700'];
const offset4700ValidRate = offset4700.valid / (offset4700.valid + offset4700.invalid) * 100;

console.log(`Key findings:`);
console.log(`1. Offset 0x4700 validity rate: ${offset4700ValidRate.toFixed(1)}%`);

if (offset4700ValidRate < 50) {
    console.log(`   ⚠️  Low validity suggests SLAB pages might NOT be contiguous!`);
    console.log(`   When we read across page boundaries, we might be reading garbage.`);
} else if (offset4700ValidRate < 80) {
    console.log(`   🤔 Mixed results - some SLABs might be contiguous, others not.`);
    console.log(`   This could depend on memory fragmentation at allocation time.`);
} else {
    console.log(`   ✅ High validity suggests SLAB pages ARE contiguous.`);
    console.log(`   The allocator seems to be getting contiguous pages.`);
}

console.log(`\n2. Comparison across offsets:`);
for (const [offset, results] of Object.entries(testResults)) {
    const total = results.valid + results.invalid;
    if (total > 0) {
        const rate = ((results.valid / total) * 100).toFixed(1);
        console.log(`   ${offset}: ${rate}% valid (${results.valid}/${total})`);
    }
}

console.log(`\n3. Implications:`);
console.log(`   - SLUB allocator typically uses order-2 allocations (16KB) for 9KB objects`);
console.log(`   - This would give 4 contiguous pages when successful`);
console.log(`   - But under memory pressure, might fall back to order-0 (single pages)`);
console.log(`   - Non-contiguous pages would cause validation failures at 0x4700`);

fs.closeSync(fd);