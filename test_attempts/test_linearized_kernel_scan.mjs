#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;

console.log('=== Linearized Kernel VA Scanner for 100% Process Discovery ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Build a linearized map of kernel VA space
class KernelVAMapper {
    constructor(fd) {
        this.fd = fd;
        this.mappings = [];
    }
    
    // Walk page tables and collect all mapped regions
    walkPGD256() {
        console.log('Walking PGD[256] page tables...\n');
        
        // Read swapper_pg_dir
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(this.fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);
        
        // Get PGD[256] entry
        const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
        if (pgd256 === 0n) {
            console.log('PGD[256] is empty!');
            return;
        }
        
        const pudTablePA = Number(pgd256 & PA_MASK & ~0xFFFn);
        console.log(`PGD[256] -> PUD table at PA 0x${pudTablePA.toString(16)}`);
        
        // Read PUD table
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(this.fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        
        // Walk all PUD entries
        for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
            const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
            if (pudEntry === 0n) continue;
            
            const pudVAStart = 0xFFFF800000000000n + (BigInt(pudIdx) << 30n);
            
            // Check if it's a 1GB block or table
            if ((pudEntry & 0x3n) === 0x1n) {
                // 1GB block mapping
                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                this.mappings.push({
                    vaStart: pudVAStart,
                    vaEnd: pudVAStart + 0x40000000n,
                    paStart: blockPA,
                    type: '1GB block'
                });
            } else if ((pudEntry & 0x3n) === 0x3n) {
                // Table pointer - walk PMD level
                this.walkPMDTable(pudVAStart, pudEntry);
            }
        }
        
        console.log(`\nFound ${this.mappings.length} mapped regions in kernel space`);
    }
    
    walkPMDTable(pudVAStart, pudEntry) {
        const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        
        try {
            fs.readSync(this.fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
            if (pmdEntry === 0n) continue;
            
            const pmdVAStart = pudVAStart + (BigInt(pmdIdx) << 21n);
            
            // Check if it's a 2MB block or table
            if ((pmdEntry & 0x3n) === 0x1n) {
                // 2MB block mapping
                const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                this.mappings.push({
                    vaStart: pmdVAStart,
                    vaEnd: pmdVAStart + 0x200000n,
                    paStart: blockPA,
                    type: '2MB block'
                });
            } else if ((pmdEntry & 0x3n) === 0x3n) {
                // Table pointer - walk PTE level
                this.walkPTETable(pmdVAStart, pmdEntry);
            }
        }
    }
    
    walkPTETable(pmdVAStart, pmdEntry) {
        const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        
        try {
            fs.readSync(this.fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        // Collect contiguous PTE mappings
        let rangeStart = null;
        let rangePA = null;
        let lastIdx = -1;
        
        for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
            const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
            
            if ((pteEntry & 0x3n) !== 0x3n) {
                // Not valid - flush any pending range
                if (rangeStart !== null) {
                    this.mappings.push({
                        vaStart: rangeStart,
                        vaEnd: pmdVAStart + (BigInt(lastIdx + 1) << 12n),
                        paStart: rangePA,
                        type: '4KB pages'
                    });
                    rangeStart = null;
                }
                continue;
            }
            
            const pagePA = Number(pteEntry & PA_MASK & ~0xFFFn);
            const pageVA = pmdVAStart + (BigInt(pteIdx) << 12n);
            
            if (rangeStart === null) {
                // Start new range
                rangeStart = pageVA;
                rangePA = pagePA;
                lastIdx = pteIdx;
            } else {
                // Check if contiguous with previous
                const expectedPA = rangePA + ((pteIdx - (Number(rangeStart - pmdVAStart) >> 12)) * PAGE_SIZE);
                if (pagePA === expectedPA) {
                    // Extend range
                    lastIdx = pteIdx;
                } else {
                    // Flush current range and start new one
                    this.mappings.push({
                        vaStart: rangeStart,
                        vaEnd: pmdVAStart + (BigInt(lastIdx + 1) << 12n),
                        paStart: rangePA,
                        type: '4KB pages'
                    });
                    rangeStart = pageVA;
                    rangePA = pagePA;
                    lastIdx = pteIdx;
                }
            }
        }
        
        // Flush final range
        if (rangeStart !== null) {
            this.mappings.push({
                vaStart: rangeStart,
                vaEnd: pmdVAStart + (BigInt(lastIdx + 1) << 12n),
                paStart: rangePA,
                type: '4KB pages'
            });
        }
    }
    
    // Create a linearized scanner that can read through VA mappings
    scanForTaskStructs() {
        console.log('\nScanning kernel VA space for task_structs...\n');
        
        const foundTasks = new Map(); // Use Map to avoid duplicates
        const stats = {
            linearMapTasks: 0,
            vmallocTasks: 0,
            totalScanned: 0
        };
        
        // Sort mappings by VA for better understanding
        this.mappings.sort((a, b) => {
            if (a.vaStart < b.vaStart) return -1;
            if (a.vaStart > b.vaStart) return 1;
            return 0;
        });
        
        // Scan each mapped region
        for (const mapping of this.mappings) {
            const size = Number(mapping.vaEnd - mapping.vaStart);

            // Identify if this is linear map or vmalloc
            // Linear map starts at 0xFFFF800040000000 (maps PA 0x40000000)
            const isLinearMap = mapping.vaStart >= 0xFFFF800040000000n &&
                                mapping.vaStart < 0xFFFF800080000000n;

            // Always scan both linear map AND vmalloc regions
            if (true) {
                console.log(`Scanning VA 0x${mapping.vaStart.toString(16)} - 0x${mapping.vaEnd.toString(16)} (${mapping.type})`);
                
                // Read the physical memory for this mapping
                const paOffset = mapping.paStart - GUEST_RAM_START;
                if (paOffset < 0 || paOffset + size > fs.fstatSync(this.fd).size) {
                    continue;
                }
                
                const buffer = Buffer.allocUnsafe(size);
                try {
                    fs.readSync(this.fd, buffer, 0, size, paOffset);
                } catch (e) {
                    continue;
                }
                
                // Scan for task_structs - use larger steps for big regions
                const step = size > 0x1000000 ? 4096 : 8; // Skip by pages for large regions
                for (let offset = 0; offset <= size - TASK_STRUCT_SIZE; offset += step) {
                    const task = this.checkForTaskStruct(buffer, offset, mapping.vaStart + BigInt(offset));
                    if (task) {
                        const key = `${task.pid}_${task.comm}`;
                        if (!foundTasks.has(key)) {
                            foundTasks.set(key, task);
                            if (isLinearMap) {
                                stats.linearMapTasks++;
                            } else {
                                stats.vmallocTasks++;
                            }
                        }
                    }
                    stats.totalScanned++;
                    
                    if (stats.totalScanned % 100000 === 0) {
                        process.stdout.write(`  Scanned ${stats.totalScanned} locations, found ${foundTasks.size} unique tasks\r`);
                    }
                }
            }
        }
        
        console.log(`\nScanning complete!`);
        console.log(`  Total unique tasks found: ${foundTasks.size}`);
        console.log(`  From linear map: ${stats.linearMapTasks}`);
        console.log(`  From vmalloc/other: ${stats.vmallocTasks}`);
        
        return Array.from(foundTasks.values());
    }
    
    checkForTaskStruct(buffer, offset, va) {
        if (offset + TASK_STRUCT_SIZE > buffer.length) return null;
        
        // Read potential PID
        const pid = buffer.readUint32LE(offset + PID_OFFSET);
        if (pid < 0 || pid > 32768) return null;
        
        // Read potential comm
        const commBuffer = buffer.subarray(offset + COMM_OFFSET, offset + COMM_OFFSET + 16);
        const comm = commBuffer.toString('ascii').split('\0')[0];
        
        if (!comm || comm.length === 0 || comm.length > 15) return null;
        if (!/^[\x20-\x7E]+$/.test(comm)) return null;
        
        return { 
            va: '0x' + va.toString(16), 
            pid, 
            comm 
        };
    }
}

// Get ground truth via SSH
async function getGroundTruth() {
    return new Promise((resolve) => {
        const client = net.createConnection({ port: 2222, host: 'localhost' }, () => {
            client.write('jff\n');
            setTimeout(() => client.write('p\n'), 100);
            setTimeout(() => client.write('ps aux | awk \'{print $2}\' | grep -E "^[0-9]+$" | sort -n\n'), 200);
            setTimeout(() => client.write('exit\n'), 300);
        });
        
        let output = '';
        client.on('data', (data) => {
            output += data.toString();
        });
        
        client.on('end', () => {
            const pids = output.match(/\d+/g)
                ?.map(p => parseInt(p))
                .filter(p => p > 0 && p <= 32768)
                .filter((p, i, a) => a.indexOf(p) === i);
            resolve(pids || []);
        });
        
        client.on('error', () => resolve([]));
    });
}

// Main execution
console.log('Building kernel VA map...\n');
const mapper = new KernelVAMapper(fd);
mapper.walkPGD256();

console.log('\n' + '='.repeat(70) + '\n');

// Show mapping summary
console.log('Mapping Summary:');
let linearMapSize = 0;
let vmallocSize = 0;
let otherSize = 0;

for (const mapping of mapper.mappings) {
    const size = Number(mapping.vaEnd - mapping.vaStart);
    if (mapping.vaStart >= 0xFFFF800000000000n && mapping.vaStart < 0xFFFF800200000000n && size > 0x10000000) {
        linearMapSize += size;
    } else if (mapping.vaStart >= 0xFFFF800008000000n && mapping.vaStart < 0xFFFF800100000000n) {
        vmallocSize += size;
    } else {
        otherSize += size;
    }
}

console.log(`  Linear map regions: ${(linearMapSize / 1024 / 1024 / 1024).toFixed(2)} GB`);
console.log(`  Vmalloc regions: ${(vmallocSize / 1024 / 1024).toFixed(2)} MB`);
console.log(`  Other regions: ${(otherSize / 1024 / 1024).toFixed(2)} MB`);

console.log('\n' + '='.repeat(70) + '\n');

const tasks = mapper.scanForTaskStructs();

console.log('\n' + '='.repeat(70) + '\n');
console.log('Results Analysis:\n');

// Get ground truth
const groundTruthPids = await getGroundTruth();
if (groundTruthPids.length > 0) {
    console.log(`Ground truth: ${groundTruthPids.length} processes from /proc`);
    
    const foundPids = tasks.map(t => t.pid);
    const matched = groundTruthPids.filter(p => foundPids.includes(p));
    const missed = groundTruthPids.filter(p => !foundPids.includes(p));
    
    console.log(`Matched: ${matched.length}/${groundTruthPids.length} (${(matched.length * 100 / groundTruthPids.length).toFixed(1)}%)`);
    
    if (missed.length > 0) {
        console.log(`\nMissed PIDs: ${missed.slice(0, 10).join(', ')}${missed.length > 10 ? '...' : ''}`);
    }
    
    if (matched.length === groundTruthPids.length) {
        console.log('\n✅ SUCCESS! Found 100% of processes!');
    } else {
        console.log(`\n❌ Still missing ${missed.length} processes (${(missed.length * 100 / groundTruthPids.length).toFixed(1)}%)`);
    }
} else {
    console.log('Could not get ground truth for comparison');
}

console.log('\nSample of found processes:');
for (const task of tasks.slice(0, 20)) {
    console.log(`  PID ${task.pid}: ${task.comm} at VA ${task.va}`);
}

console.log('\n' + '='.repeat(70) + '\n');
console.log('Conclusion:');
console.log('By scanning the linearized kernel VA space, we can access');
console.log('both the linear map AND vmalloc regions where task_structs live.');
console.log('This should give us 100% process discovery!');

fs.closeSync(fd);