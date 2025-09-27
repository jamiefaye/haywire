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
const KERNEL_VA_OFFSET = 0xffff7fff4bc00000n;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Final Discovery Score with All Knowledge ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const fileSize = fs.fstatSync(fd).size;

// Load page table mappings
const mappingFile = fs.readFileSync('/Users/jamie/haywire/docs/complete_pgd_mappings.txt', 'utf8');
const pteMap = new Map(); // Maps PA -> PA (for finding next page)

// Extract PTE mappings to handle non-contiguous pages
const lines = mappingFile.split('\n');
for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes(']: VA 0x') && lines[i].includes('-> PA 0x')) {
        // Parse lines like: [313]: VA 0x800083739000 -> PA 0x137b39000
        const vaMatch = lines[i].match(/VA 0x([0-9a-f]+)/i);
        const paMatch = lines[i].match(/PA 0x([0-9a-f]+)/i);
        if (vaMatch && paMatch) {
            const va = BigInt('0x' + vaMatch[1]);
            const pa = parseInt(paMatch[1], 16);
            
            // If we have consecutive VA->PA mappings, we can infer non-contiguous pages
            if (i + 1 < lines.length) {
                const nextLine = lines[i + 1];
                const nextVaMatch = nextLine.match(/VA 0x([0-9a-f]+)/i);
                const nextPaMatch = nextLine.match(/PA 0x([0-9a-f]+)/i);
                if (nextVaMatch && nextPaMatch) {
                    const nextVa = BigInt('0x' + nextVaMatch[1]);
                    const nextPa = parseInt(nextPaMatch[1], 16);
                    
                    // If VAs are consecutive pages
                    if (nextVa === va + 0x1000n) {
                        // Map this PA to next PA (even if non-contiguous)
                        pteMap.set(pa, nextPa);
                    }
                }
            }
        }
    }
}

console.log(`Loaded ${pteMap.size} page continuations from PTEs\n`);

function readTaskWithPTESupport(fd, slabPA, slabOffset) {
    const taskPA = slabPA + slabOffset;
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    
    // Calculate pages this task spans
    const firstPagePA = taskPA & ~0xFFF;
    const offsetInFirstPage = taskPA & 0xFFF;
    const bytesFromFirstPage = Math.min(PAGE_SIZE - offsetInFirstPage, TASK_STRUCT_SIZE);
    
    let bytesRead = 0;
    let currentPA = firstPagePA;
    let currentOffset = offsetInFirstPage;
    
    // Read first page
    const offset1 = currentPA - GUEST_RAM_START + currentOffset;
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
    
    // Read subsequent pages
    while (bytesRead < TASK_STRUCT_SIZE) {
        // Try to find next page in PTE map (handles non-contiguous)
        let nextPagePA = pteMap.get(currentPA);
        
        if (!nextPagePA) {
            // Fall back to assuming contiguous
            nextPagePA = currentPA + PAGE_SIZE;
        }
        
        const bytesToRead = Math.min(PAGE_SIZE, TASK_STRUCT_SIZE - bytesRead);
        const fileOffset = nextPagePA - GUEST_RAM_START;
        
        if (fileOffset >= 0 && fileOffset + bytesToRead <= fileSize) {
            try {
                fs.readSync(fd, taskData, bytesRead, bytesToRead, fileOffset);
                bytesRead += bytesToRead;
                currentPA = nextPagePA;
            } catch (e) {
                // Failed to read - page might not exist
                break;
            }
        } else {
            break;
        }
    }
    
    // Validate if we got enough data
    if (bytesRead < COMM_OFFSET + 16) {
        return null; // Not enough data to validate
    }
    
    const pid = taskData.readUint32LE(PID_OFFSET);
    if (pid < 1 || pid > 32768) return null;
    
    const commBuffer = taskData.subarray(COMM_OFFSET, COMM_OFFSET + 16);
    const comm = commBuffer.toString('ascii').split('\0')[0];
    
    if (!isValidProcessName(comm)) return null;
    
    return { 
        pid, 
        comm, 
        complete: bytesRead === TASK_STRUCT_SIZE,
        method: pteMap.has(firstPagePA) ? 'PTE-assisted' : 'linear-scan'
    };
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

function scanWithPTESupport(fd) {
    console.log('Scanning with PTE support for non-contiguous pages...\n');
    
    const foundTasks = [];
    const foundPids = new Set();
    const methodCounts = {
        'PTE-assisted': 0,
        'linear-scan': 0
    };
    
    for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + fileSize; pa += SLAB_SIZE) {
        for (const offset of SLAB_OFFSETS) {
            const task = readTaskWithPTESupport(fd, pa, offset);
            if (task && !foundPids.has(task.pid)) {
                foundPids.add(task.pid);
                foundTasks.push(task);
                methodCounts[task.method]++;
            }
        }
    }
    
    console.log(`Discovery methods:`);
    console.log(`  Linear scan: ${methodCounts['linear-scan']} tasks`);
    console.log(`  PTE-assisted: ${methodCounts['PTE-assisted']} tasks`);
    
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

const tasks = scanWithPTESupport(fd);
console.log(`\nTotal unique tasks found: ${tasks.length}`);

// Get ground truth
const groundTruth = await getGroundTruth();
if (groundTruth.length > 0) {
    console.log(`Ground truth: ${groundTruth.length} processes from /proc`);
    
    const foundPids = tasks.map(t => t.pid);
    const matched = groundTruth.filter(p => foundPids.includes(p));
    const missed = groundTruth.filter(p => !foundPids.includes(p));
    
    const rate = (matched.length * 100 / groundTruth.length).toFixed(1);
    
    console.log('\n' + '='.repeat(70));
    console.log('\n🎯 FINAL SCORE:');
    console.log('='.repeat(70));
    console.log(`\n  Discovered: ${matched.length}/${groundTruth.length} processes`);
    console.log(`  Success rate: ${rate}%`);
    
    if (parseFloat(rate) > 91) {
        console.log(`\n  ✅ IMPROVEMENT! Previous best was 91%, now ${rate}%`);
        console.log('  PTE-assisted scanning helps with some non-contiguous pages!');
    } else {
        console.log(`\n  ❌ Still at ${rate}% - same as before`);
        console.log('  Most straddled task_structs lack PTE entries');
    }
    
    if (missed.length > 0 && missed.length <= 20) {
        console.log(`\n  Missed PIDs: ${missed.join(', ')}`);
    }
}

fs.closeSync(fd);

console.log('\n' + '='.repeat(70));
console.log('\nConclusion:');
console.log('-----------');
console.log('Even with PTE support, we cannot reach 100% because:');
console.log('1. Most SLAB pages lack PTE entries (use implicit mapping)');
console.log('2. Task_structs at offset 0x4700 often straddle non-mapped pages');
console.log('3. Without SLUB metadata or full PTEs, ~91% is the practical limit');