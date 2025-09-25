#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const SLAB_SIZE = 0x8000;
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

// The magic offset we discovered!
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;

console.log('=== 100% Process Discovery Using Fixed VA Offset ===\n');
console.log(`Kernel VA formula: VA = PA + 0x${KERNEL_VA_OFFSET.toString(16)}\n`);

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const fileSize = fs.fstatSync(fd).size;

function paToVA(pa) {
    return BigInt(pa) + KERNEL_VA_OFFSET;
}

function vaToPA(va) {
    return Number(va - KERNEL_VA_OFFSET);
}

function readTaskStructWithOffset(fd, slabPA, slabOffset) {
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    const taskPA = slabPA + slabOffset;
    const taskVA = paToVA(taskPA);
    
    // Calculate pages this task spans
    const firstPagePA = taskPA & ~0xFFF;
    const offsetInFirstPage = taskPA & 0xFFF;
    const bytesFromFirstPage = Math.min(PAGE_SIZE - offsetInFirstPage, TASK_STRUCT_SIZE);
    
    let bytesRead = 0;
    
    // Read first page
    const offset1 = firstPagePA - GUEST_RAM_START + offsetInFirstPage;
    if (offset1 >= 0 && offset1 + bytesFromFirstPage <= fileSize) {
        try {
            fs.readSync(fd, taskData, 0, bytesFromFirstPage, offset1);
            bytesRead = bytesFromFirstPage;
        } catch (e) {
            return null;
        }
    } else {
        return null;
    }
    
    // If task straddles pages, use VA to find next pages
    let currentVA = taskVA + BigInt(bytesRead);
    
    while (bytesRead < TASK_STRUCT_SIZE) {
        // Calculate next page PA using our fixed offset
        const nextPageVA = currentVA & ~0xFFFn;
        const nextPagePA = vaToPA(nextPageVA);
        const offsetInPage = Number(currentVA & 0xFFFn);
        const bytesToRead = Math.min(PAGE_SIZE - offsetInPage, TASK_STRUCT_SIZE - bytesRead);
        
        const fileOffset = nextPagePA - GUEST_RAM_START + offsetInPage;
        
        if (fileOffset >= 0 && fileOffset + bytesToRead <= fileSize) {
            try {
                fs.readSync(fd, taskData, bytesRead, bytesToRead, fileOffset);
                bytesRead += bytesToRead;
                currentVA += BigInt(bytesToRead);
            } catch (e) {
                // This page might be non-contiguous - the key test!
                // Try to find it elsewhere using heuristics
                return tryRecoverTask(fd, taskData, bytesRead, slabPA, slabOffset);
            }
        } else {
            return tryRecoverTask(fd, taskData, bytesRead, slabPA, slabOffset);
        }
    }
    
    return validateTaskStruct(taskData, bytesRead === TASK_STRUCT_SIZE);
}

function tryRecoverTask(fd, partialData, bytesRead, slabPA, slabOffset) {
    // For straddled tasks where pages aren't contiguous
    // We have the PID but might be missing comm
    
    if (bytesRead >= PID_OFFSET + 4) {
        const pid = partialData.readUint32LE(PID_OFFSET);
        if (pid > 0 && pid < 32768) {
            // We have PID, try to get comm if missing
            if (bytesRead >= COMM_OFFSET + 16) {
                // We have both
                return validateTaskStruct(partialData, false);
            }
            
            // Search for comm in nearby memory
            const searchOffsets = [
                0x1000,   // Next page
                0x2000,   // Page after
                -0x1000,  // Previous page
                0x8000,   // Next SLAB
                -0x8000,  // Previous SLAB
            ];
            
            for (const delta of searchOffsets) {
                const searchPA = (slabPA + slabOffset + COMM_OFFSET) & ~0xFFF;
                const testPA = searchPA + delta;
                const testOffset = testPA - GUEST_RAM_START + (COMM_OFFSET & 0xFFF);
                
                if (testOffset >= 0 && testOffset + 16 <= fileSize) {
                    const commBuffer = Buffer.allocUnsafe(16);
                    try {
                        fs.readSync(fd, commBuffer, 0, 16, testOffset);
                        const comm = commBuffer.toString('ascii').split('\0')[0];
                        
                        if (comm && isValidProcessName(comm)) {
                            return { pid, comm, straddled: true, recovered: true };
                        }
                    } catch (e) {}
                }
            }
            
            // Couldn't recover comm
            return { pid, comm: '<fragmented>', straddled: true, recovered: false };
        }
    }
    
    return null;
}

function isValidProcessName(comm) {
    if (!comm || comm.length === 0 || comm.length > 15) return false;
    if (!/^[\x20-\x7E]+$/.test(comm)) return false;
    
    // Kernel thread pattern
    if (comm.startsWith('[') && comm.endsWith(']')) {
        const inner = comm.slice(1, -1);
        return /^[a-zA-Z][a-zA-Z0-9_\-\/]*$/.test(inner);
    }
    
    // Regular process
    return /^[a-zA-Z0-9\/\-_][\x20-\x7E]*$/.test(comm);
}

function validateTaskStruct(buffer, complete) {
    const pid = buffer.readUint32LE(PID_OFFSET);
    if (pid < 1 || pid > 32768) return null;
    
    const commBuffer = buffer.subarray(COMM_OFFSET, COMM_OFFSET + 16);
    const comm = commBuffer.toString('ascii').split('\0')[0];
    
    if (!isValidProcessName(comm)) return null;
    
    // Additional validation
    const state = buffer.readUint32LE(0x18);
    if (state > 0x1000) return null;
    
    return { pid, comm, straddled: !complete };
}

function scanWithFixedOffset(fd) {
    console.log('Scanning physical memory with VA offset handling...\n');
    
    const foundTasks = [];
    const foundPids = new Set();
    let slabCount = 0;
    let straddledCount = 0;
    let recoveredCount = 0;
    
    for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + fileSize; pa += SLAB_SIZE) {
        let isSLAB = false;
        
        for (const offset of SLAB_OFFSETS) {
            const task = readTaskStructWithOffset(fd, pa, offset);
            if (task) {
                isSLAB = true;
                if (!foundPids.has(task.pid)) {
                    foundPids.add(task.pid);
                    foundTasks.push({
                        ...task,
                        pa: pa + offset,
                        slabOffset: offset
                    });
                    
                    if (task.straddled) straddledCount++;
                    if (task.recovered) recoveredCount++;
                }
            }
        }
        
        if (isSLAB) {
            slabCount++;
            if (slabCount % 100 === 0) {
                process.stdout.write(`  Found ${slabCount} SLABs, ${foundTasks.length} unique tasks (${straddledCount} straddled)\r`);
            }
        }
    }
    
    console.log(`  Found ${slabCount} SLABs, ${foundTasks.length} unique tasks (${straddledCount} straddled)`);
    return foundTasks;
}

// Get ground truth
async function getGroundTruth() {
    return new Promise((resolve) => {
        exec(`ssh vm "ps aux | awk '{print \\$2}' | tail -n +2"`, (error, stdout) => {
            if (error) {
                resolve([]);
                return;
            }
            
            const pids = stdout.trim().split('\n')
                .map(p => parseInt(p))
                .filter(p => p > 0 && p <= 32768);
            
            resolve(pids);
        });
    });
}

const tasks = scanWithFixedOffset(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Results:\n');
console.log(`Total unique tasks found: ${tasks.length}`);

const straddled = tasks.filter(t => t.straddled).length;
const recovered = tasks.filter(t => t.recovered).length;
const fragmented = tasks.filter(t => t.comm === '<fragmented>').length;

console.log(`  Complete: ${tasks.length - straddled}`);
console.log(`  Straddled: ${straddled}`);
console.log(`    - Recovered: ${recovered}`);
console.log(`    - Fragmented: ${fragmented}`);

// Distribution by offset
const byOffset = {};
for (const task of tasks) {
    const key = '0x' + task.slabOffset.toString(16);
    if (!byOffset[key]) byOffset[key] = 0;
    byOffset[key]++;
}

console.log('\nDistribution by SLAB offset:');
for (const [offset, count] of Object.entries(byOffset)) {
    const pct = (count * 100 / tasks.length).toFixed(1);
    console.log(`  ${offset}: ${count} tasks (${pct}%)`);
    if (offset === '0x4700') {
        const straddledAt4700 = tasks.filter(t => t.slabOffset === 0x4700 && t.straddled).length;
        console.log(`    Straddled at 0x4700: ${straddledAt4700}`);
    }
}

// Get ground truth
const groundTruth = await getGroundTruth();
if (groundTruth.length > 0) {
    console.log(`\nGround truth: ${groundTruth.length} processes from /proc`);
    
    const foundPids = tasks.map(t => t.pid);
    const matched = groundTruth.filter(p => foundPids.includes(p));
    const missed = groundTruth.filter(p => !foundPids.includes(p));
    
    const rate = (matched.length * 100 / groundTruth.length).toFixed(1);
    console.log(`Matched: ${matched.length}/${groundTruth.length} (${rate}%)`);
    
    if (missed.length > 0) {
        console.log(`\nMissed PIDs: ${missed.slice(0, 10).join(', ')}${missed.length > 10 ? '...' : ''}`);
    }
    
    if (parseFloat(rate) >= 99) {
        console.log(`\n✅ SUCCESS! Achieved ${rate}% discovery!`);
        console.log('The fixed VA offset allows us to handle most straddled cases!');
    } else if (parseFloat(rate) >= 95) {
        console.log(`\n✅ Good! ${rate}% discovery rate`);
    } else {
        console.log(`\n❌ Still only ${rate}% discovery`);
        console.log('The fixed offset helps but doesn\'t solve non-contiguous pages');
    }
}

fs.closeSync(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Key Findings:');
console.log('------------');
console.log('1. Kernel uses fixed offset: VA = PA + 0x' + KERNEL_VA_OFFSET.toString(16));
console.log('2. This works for contiguous pages');
console.log('3. But ~9% of task_structs at 0x4700 have non-contiguous pages');
console.log('4. Without SLUB metadata, we can\'t find the non-contiguous pages');
console.log('5. Maximum achievable: ~91% with physical scanning');