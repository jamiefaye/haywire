#!/usr/bin/env node

import fs from 'fs';
import net from 'net';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const SLAB_SIZE = 0x8000; // 32KB
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== Hybrid Scanner: Physical Scan + VA Straddling ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Build VA->PA mapping for handling straddling
class PageTableMapper {
    constructor(fd) {
        this.fd = fd;
        this.vaToPA = new Map();
    }
    
    buildMapping() {
        console.log('Building VA->PA mapping for straddle handling...\n');
        
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(this.fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);
        
        const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
        if (pgd256 === 0n) return;
        
        const pudTablePA = Number(pgd256 & PA_MASK & ~0xFFFn);
        this.walkPUD(pudTablePA, 0xFFFF800000000000n);
        
        console.log(`Mapped ${this.vaToPA.size} pages\n`);
    }
    
    walkPUD(pudTablePA, pudVABase) {
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
            const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
            if (pudEntry === 0n) continue;
            
            const pudVA = pudVABase + (BigInt(pudIdx) << 30n);
            
            if ((pudEntry & 0x3n) === 0x1n) {
                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                for (let offset = 0; offset < 0x40000000; offset += PAGE_SIZE) {
                    this.vaToPA.set(pudVA + BigInt(offset), blockPA + offset);
                }
            } else if ((pudEntry & 0x3n) === 0x3n) {
                const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
                this.walkPMD(pmdTablePA, pudVA);
            }
        }
    }
    
    walkPMD(pmdTablePA, pmdVABase) {
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
            if (pmdEntry === 0n) continue;
            
            const pmdVA = pmdVABase + (BigInt(pmdIdx) << 21n);
            
            if ((pmdEntry & 0x3n) === 0x1n) {
                const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                for (let offset = 0; offset < 0x200000; offset += PAGE_SIZE) {
                    this.vaToPA.set(pmdVA + BigInt(offset), blockPA + offset);
                }
            } else if ((pmdEntry & 0x3n) === 0x3n) {
                const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
                this.walkPTE(pteTablePA, pmdVA);
            }
        }
    }
    
    walkPTE(pteTablePA, pteVABase) {
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
            const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
            if ((pteEntry & 0x3n) !== 0x3n) continue;
            
            const pageVA = pteVABase + (BigInt(pteIdx) << 12n);
            const pagePA = Number(pteEntry & PA_MASK & ~0xFFFn);
            this.vaToPA.set(pageVA, pagePA);
        }
    }
    
    lookupVA(va) {
        const pageVA = va & ~0xFFFn;
        const pageOffset = Number(va & 0xFFFn);
        const pagePA = this.vaToPA.get(pageVA);
        if (pagePA === undefined) return null;
        return pagePA + pageOffset;
    }
}

// Check if a PA looks like a SLAB page start
function checkIfSLABStart(fd, pa) {
    const offset = pa - GUEST_RAM_START;
    if (offset < 0 || offset + SLAB_SIZE > fs.fstatSync(fd).size) {
        return false;
    }

    // Check first task_struct at offset 0
    const buffer = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    try {
        fs.readSync(fd, buffer, 0, TASK_STRUCT_SIZE, offset);
    } catch (e) {
        return false;
    }

    const pid = buffer.readUint32LE(PID_OFFSET);
    const comm = buffer.subarray(COMM_OFFSET, COMM_OFFSET + 16).toString('ascii').split('\0')[0];

    // Much stricter validation
    if (pid < 1 || pid > 32768) return false;
    if (!comm || comm.length === 0 || comm.length > 15) return false;

    // Check for kernel thread pattern [name]
    if (comm.startsWith('[') && comm.endsWith(']')) {
        // Kernel thread - verify it's a known pattern
        const inner = comm.slice(1, -1);
        if (/^[a-zA-Z][a-zA-Z0-9_\-\/]*$/.test(inner)) {
            return true;
        }
    }

    // Check for regular process
    if (/^[a-zA-Z0-9\/][\x20-\x7E]*$/.test(comm)) {
        // Also check state field is reasonable
        const state = buffer.readUint32LE(0x18);
        if (state < 0x1000) {
            return true;
        }
    }

    return false;
}

// Read task_struct handling straddling
function readTaskWithStraddling(fd, mapper, slabPA, slabOffset) {
    // Simple case: task doesn't straddle
    if (slabOffset + TASK_STRUCT_SIZE <= (((slabPA + slabOffset) & ~0xFFF) + PAGE_SIZE)) {
        const offset = slabPA + slabOffset - GUEST_RAM_START;
        if (offset < 0 || offset + TASK_STRUCT_SIZE > fs.fstatSync(fd).size) {
            return null;
        }
        
        const buffer = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
        try {
            fs.readSync(fd, buffer, 0, TASK_STRUCT_SIZE, offset);
        } catch (e) {
            return null;
        }
        
        return checkTaskStruct(buffer, 0, false);
    }
    
    // Complex case: task straddles pages
    // Use VA mapping to find next pages
    const slabVA = BigInt(slabPA - GUEST_RAM_START) + 0xFFFF800040000000n;
    const taskVA = slabVA + BigInt(slabOffset);
    
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    let bytesRead = 0;
    
    while (bytesRead < TASK_STRUCT_SIZE) {
        const currentVA = taskVA + BigInt(bytesRead);
        const currentPA = mapper.lookupVA(currentVA);
        
        if (!currentPA) {
            // Can't find mapping - fall back to assuming contiguous
            const assumedPA = slabPA + slabOffset + bytesRead;
            const pageOffset = assumedPA - GUEST_RAM_START;
            const pageStartOffset = bytesRead & ~0xFFF;
            const bytesInPage = Math.min(PAGE_SIZE - (bytesRead & 0xFFF), TASK_STRUCT_SIZE - bytesRead);
            
            try {
                fs.readSync(fd, taskData, bytesRead, bytesInPage, pageOffset);
            } catch (e) {
                return null;
            }
            bytesRead += bytesInPage;
        } else {
            // Use mapped PA
            const pageOffset = currentPA - GUEST_RAM_START;
            const bytesInPage = Math.min(PAGE_SIZE - (Number(currentVA) & 0xFFF), TASK_STRUCT_SIZE - bytesRead);
            
            try {
                fs.readSync(fd, taskData, bytesRead, bytesInPage, pageOffset);
            } catch (e) {
                return null;
            }
            bytesRead += bytesInPage;
        }
    }
    
    return checkTaskStruct(taskData, 0, true);
}

// Validate task_struct
function checkTaskStruct(buffer, offset, straddled) {
    const pid = buffer.readUint32LE(offset + PID_OFFSET);
    if (pid < 1 || pid > 32768) return null;
    
    const commBuffer = buffer.subarray(offset + COMM_OFFSET, offset + COMM_OFFSET + 16);
    const comm = commBuffer.toString('ascii').split('\0')[0];
    
    if (!comm || comm.length === 0 || comm.length > 15) return null;
    if (!/^[\x20-\x7E]+$/.test(comm)) return null;
    
    // More strict validation for regular processes
    if (!comm.startsWith('[')) {
        // User process - should start with letter, number, or /
        if (!/^[a-zA-Z0-9\/]/.test(comm)) return null;
    }
    
    return { pid, comm, straddled };
}

// Main scanning function
function hybridScan(fd, mapper) {
    console.log('Scanning physical memory for SLAB pages...\n');
    
    const foundTasks = [];
    const checkedPAs = new Set();
    let slabCount = 0;
    
    // Scan physical memory for SLAB pages
    for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + 0x180000000; pa += SLAB_SIZE) {
        if (checkedPAs.has(pa)) continue;
        
        if (checkIfSLABStart(fd, pa)) {
            slabCount++;
            checkedPAs.add(pa);
            
            // Try all three offsets
            for (const offset of SLAB_OFFSETS) {
                const task = readTaskWithStraddling(fd, mapper, pa, offset);
                if (task) {
                    foundTasks.push({
                        ...task,
                        pa: pa + offset,
                        slabPA: pa,
                        slabOffset: offset
                    });
                }
            }
            
            if (slabCount % 10 === 0) {
                process.stdout.write(`  Found ${slabCount} SLABs, ${foundTasks.length} tasks\r`);
            }
        }
    }
    
    console.log(`  Found ${slabCount} SLABs, ${foundTasks.length} tasks`);
    return foundTasks;
}

// Get ground truth using SSH with proper authentication
async function getGroundTruth() {
    return new Promise((resolve, reject) => {
        // Use ssh with the vm alias that's already set up
        exec(`ssh vm "ps aux | awk '{print \\$2}' | tail -n +2"`, (error, stdout, stderr) => {
            if (error) {
                console.log('Failed to get ground truth via SSH');
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

// Main execution
const mapper = new PageTableMapper(fd);
mapper.buildMapping();

const tasks = hybridScan(fd, mapper);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Results:\n');
console.log(`Total tasks found: ${tasks.length}`);

const straddledCount = tasks.filter(t => t.straddled).length;
console.log(`Tasks that straddled pages: ${straddledCount}`);

// Group by SLAB offset
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
const groundTruthPids = await getGroundTruth();
if (groundTruthPids.length > 0) {
    console.log(`\nGround truth: ${groundTruthPids.length} processes from /proc`);
    
    const foundPids = tasks.map(t => t.pid);
    const matched = groundTruthPids.filter(p => foundPids.includes(p));
    const missed = groundTruthPids.filter(p => !foundPids.includes(p));
    
    console.log(`Matched: ${matched.length}/${groundTruthPids.length} (${(matched.length * 100 / groundTruthPids.length).toFixed(1)}%)`);
    
    if (missed.length > 0) {
        console.log(`\nMissed PIDs: ${missed.slice(0, 10).join(', ')}${missed.length > 10 ? '...' : ''}`);
        
        // Show which offsets we missed
        console.log('\nAnalyzing missed processes:');
        console.log('The missed ones are likely at offset 0x4700 with non-contiguous pages');
    }
    
    if (matched.length === groundTruthPids.length) {
        console.log('\n✅ SUCCESS! Found 100% of processes!');
        console.log('The hybrid approach with VA mapping handles straddling!');
    }
} else {
    console.log('Could not get ground truth');
}

console.log('\nSample of found processes:');
for (const task of tasks.slice(0, 20)) {
    const offsetStr = '0x' + task.slabOffset.toString(16).padStart(4, '0');
    console.log(`  PID ${task.pid.toString().padStart(5)}: ${task.comm.padEnd(15)} at offset ${offsetStr} ${task.straddled ? '[STRADDLED]' : ''}`);
}

fs.closeSync(fd);