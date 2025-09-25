#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const SLAB_SIZE = 0x8000; // 32KB
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== 100% Process Discovery via Smart Physical Scanning ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const fileSize = fs.fstatSync(fd).size;

console.log(`Memory file size: ${(fileSize / 1024 / 1024 / 1024).toFixed(2)} GB\n`);

// For task at offset 0x4700 that straddles:
// - Bytes 0x0000-0x08FF in page at offset 0x4000 (2304 bytes) 
// - Bytes 0x0900-0x18FF in page at offset 0x5000 (4096 bytes)
// - Bytes 0x1900-0x237F in page at offset 0x6000 (2688 bytes)

function readTaskStruct(fd, slabPA, slabOffset) {
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    
    // Calculate how task_struct spans pages
    const taskStart = slabPA + slabOffset;
    const page1Start = taskStart & ~0xFFF;
    const offsetInPage1 = taskStart & 0xFFF;
    
    // How many bytes from each page?
    const bytesFromPage1 = Math.min(PAGE_SIZE - offsetInPage1, TASK_STRUCT_SIZE);
    const bytesFromPage2 = Math.min(PAGE_SIZE, Math.max(0, TASK_STRUCT_SIZE - bytesFromPage1));
    const bytesFromPage3 = Math.max(0, TASK_STRUCT_SIZE - bytesFromPage1 - bytesFromPage2);
    
    let success = true;
    let bytesRead = 0;
    
    // Read from first page
    if (bytesFromPage1 > 0) {
        const offset1 = page1Start - GUEST_RAM_START + offsetInPage1;
        if (offset1 >= 0 && offset1 + bytesFromPage1 <= fileSize) {
            try {
                fs.readSync(fd, taskData, 0, bytesFromPage1, offset1);
                bytesRead += bytesFromPage1;
            } catch (e) {
                success = false;
            }
        } else {
            success = false;
        }
    }
    
    // For straddling cases, try multiple strategies
    if (bytesFromPage2 > 0 && success) {
        // Strategy 1: Assume contiguous (works for 91%)
        const page2Start = page1Start + PAGE_SIZE;
        const offset2 = page2Start - GUEST_RAM_START;
        
        if (offset2 >= 0 && offset2 + bytesFromPage2 <= fileSize) {
            try {
                fs.readSync(fd, taskData, bytesRead, bytesFromPage2, offset2);
                bytesRead += bytesFromPage2;
            } catch (e) {
                // Strategy 2: Try scanning for the rest of the task
                success = false;
            }
        } else {
            success = false;
        }
    }
    
    // Read third page if needed
    if (bytesFromPage3 > 0 && success) {
        const page3Start = page1Start + 2 * PAGE_SIZE;
        const offset3 = page3Start - GUEST_RAM_START;
        
        if (offset3 >= 0 && offset3 + bytesFromPage3 <= fileSize) {
            try {
                fs.readSync(fd, taskData, bytesRead, bytesFromPage3, offset3);
                bytesRead += bytesFromPage3;
            } catch (e) {
                success = false;
            }
        } else {
            success = false;
        }
    }
    
    if (!success || bytesRead < TASK_STRUCT_SIZE) {
        // For the 9% case: Try heuristics
        // If we at least got the PID, try to find the comm field
        if (bytesRead >= PID_OFFSET + 4) {
            const pid = taskData.readUint32LE(PID_OFFSET);
            if (pid > 0 && pid < 32768) {
                // We have a valid PID but missing comm
                // Strategy: Search nearby pages for the comm
                return tryRecoverStraddled(fd, taskData, bytesRead, pid, slabPA, slabOffset);
            }
        }
        return null;
    }
    
    return validateTaskStruct(taskData);
}

function tryRecoverStraddled(fd, partialData, bytesRead, pid, slabPA, slabOffset) {
    // For offset 0x4700, comm is at 0x970 = 2416 bytes from start
    // This would be in the second page
    
    if (bytesRead >= COMM_OFFSET + 16) {
        // We already have comm, validate it
        return validateTaskStruct(partialData);
    }
    
    // Try to find comm in nearby memory
    // This is a heuristic for the 9% non-contiguous case
    // In practice, the pages might be within a few MB of each other
    
    const searchRanges = [
        0x1000,   // Next page
        -0x1000,  // Previous page  
        0x8000,   // Next SLAB
        -0x8000,  // Previous SLAB
        0x10000,  // Further out
        -0x10000,
    ];
    
    const taskStart = slabPA + slabOffset;
    const commPageOffset = (taskStart + COMM_OFFSET) & ~0xFFF;
    
    for (const delta of searchRanges) {
        const searchPA = commPageOffset + delta;
        const searchOffset = searchPA - GUEST_RAM_START + (COMM_OFFSET & 0xFFF);
        
        if (searchOffset >= 0 && searchOffset + 16 <= fileSize) {
            const commBuffer = Buffer.allocUnsafe(16);
            try {
                fs.readSync(fd, commBuffer, 0, 16, searchOffset);
                const comm = commBuffer.toString('ascii').split('\0')[0];
                
                if (comm && comm.length > 0 && comm.length <= 15 && /^[\x20-\x7E]+$/.test(comm)) {
                    // Found a plausible comm field
                    // Check if it matches known patterns
                    if (isLikelyProcessName(comm)) {
                        // Reconstruct complete task_struct
                        partialData.set(commBuffer, COMM_OFFSET);
                        return { pid, comm, recovered: true };
                    }
                }
            } catch (e) {}
        }
    }
    
    // Last resort: Accept just the PID
    return { pid, comm: '<fragmented>', recovered: false };
}

function isLikelyProcessName(comm) {
    // Common patterns
    if (comm.startsWith('[') && comm.endsWith(']')) return true; // Kernel thread
    if (comm.startsWith('systemd')) return true;
    if (comm.startsWith('kworker')) return true;
    if (/^[a-zA-Z][a-zA-Z0-9_\-]*$/.test(comm)) return true;
    if (comm.includes('/')) return true; // Path
    return false;
}

function validateTaskStruct(buffer) {
    const pid = buffer.readUint32LE(PID_OFFSET);
    if (pid < 1 || pid > 32768) return null;
    
    const commBuffer = buffer.subarray(COMM_OFFSET, COMM_OFFSET + 16);
    const comm = commBuffer.toString('ascii').split('\0')[0];
    
    if (!comm || comm.length === 0 || comm.length > 15) return null;
    if (!/^[\x20-\x7E]+$/.test(comm)) return null;
    
    // Additional validation
    const state = buffer.readUint32LE(0x18);
    if (state > 0x1000) return null;
    
    return { pid, comm };
}

function scanPhysicalMemory(fd) {
    console.log('Scanning physical memory for task_structs...\n');
    
    const foundTasks = [];
    const foundPids = new Set();
    let slabCount = 0;
    
    for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + fileSize; pa += SLAB_SIZE) {
        // Quick check if this looks like a SLAB
        let isSLAB = false;
        
        for (const offset of SLAB_OFFSETS) {
            const task = readTaskStruct(fd, pa, offset);
            if (task) {
                isSLAB = true;
                if (!foundPids.has(task.pid)) {
                    foundPids.add(task.pid);
                    foundTasks.push({
                        ...task,
                        pa: pa + offset,
                        slabOffset: offset
                    });
                }
            }
        }
        
        if (isSLAB) {
            slabCount++;
            if (slabCount % 50 === 0) {
                process.stdout.write(`  Found ${slabCount} SLABs, ${foundTasks.length} unique tasks\r`);
            }
        }
    }
    
    console.log(`  Found ${slabCount} SLABs, ${foundTasks.length} unique tasks`);
    return foundTasks;
}

// Get ground truth
async function getGroundTruth() {
    return new Promise((resolve) => {
        exec(`ssh vm "ps aux | awk '{print \\$2\" \\"\\$11}' | tail -n +2"`, (error, stdout) => {
            if (error) {
                resolve([]);
                return;
            }
            
            const processes = stdout.trim().split('\n').map(line => {
                const [pid, comm] = line.split(' ', 2);
                return { pid: parseInt(pid), comm };
            }).filter(p => p.pid > 0);
            
            resolve(processes);
        });
    });
}

const tasks = scanPhysicalMemory(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Results:\n');
console.log(`Total unique tasks found: ${tasks.length}`);

const recovered = tasks.filter(t => t.recovered).length;
const fragmented = tasks.filter(t => t.comm === '<fragmented>').length;

console.log(`  Regular: ${tasks.length - recovered - fragmented}`);
console.log(`  Recovered: ${recovered} (used heuristics for non-contiguous pages)`);
console.log(`  Fragmented: ${fragmented} (couldn't recover comm field)`);

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
}

// Get ground truth
const groundTruth = await getGroundTruth();
if (groundTruth.length > 0) {
    console.log(`\nGround truth: ${groundTruth.length} processes from /proc`);
    
    const foundPids = tasks.map(t => t.pid);
    const groundPids = groundTruth.map(p => p.pid);
    
    const matched = groundPids.filter(p => foundPids.includes(p));
    const missed = groundPids.filter(p => !foundPids.includes(p));
    
    console.log(`Matched: ${matched.length}/${groundPids.length} (${(matched.length * 100 / groundPids.length).toFixed(1)}%)`);
    
    if (missed.length > 0) {
        console.log(`\nMissed PIDs: ${missed.slice(0, 10).join(', ')}${missed.length > 10 ? '...' : ''}`);
        
        // Check which processes we missed
        console.log('\nMissed processes:');
        for (const pid of missed.slice(0, 5)) {
            const proc = groundTruth.find(p => p.pid === pid);
            if (proc) {
                console.log(`  PID ${pid}: ${proc.comm}`);
            }
        }
    }
    
    const rate = (matched.length * 100 / groundPids.length).toFixed(1);
    if (rate >= 99) {
        console.log(`\n✅ SUCCESS! Achieved ${rate}% discovery rate!`);
    } else if (rate >= 95) {
        console.log(`\n✅ Good! Achieved ${rate}% discovery rate!`);
    } else {
        console.log(`\n⚠️  Only ${rate}% discovery rate`);
    }
} else {
    console.log('\nCould not get ground truth for comparison');
}

console.log('\nSample of found processes:');
for (const task of tasks.slice(0, 20)) {
    const offsetStr = '0x' + task.slabOffset.toString(16).padStart(4, '0');
    const status = task.recovered ? ' [RECOVERED]' : task.comm === '<fragmented>' ? ' [FRAGMENTED]' : '';
    console.log(`  PID ${task.pid.toString().padStart(5)}: ${task.comm.padEnd(20)} at offset ${offsetStr}${status}`);
}

fs.closeSync(fd);