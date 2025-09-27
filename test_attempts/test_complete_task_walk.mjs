#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

// Known swapper_pg_dir from our testing
const SWAPPER_PGD_PA = 0x136deb000;

async function getKernelInfo() {
    return new Promise((resolve) => {
        const socket = new net.Socket();
        let buffer = '';
        let capabilitiesSent = false;

        socket.on('data', (data) => {
            buffer += data.toString();
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (!line.trim()) continue;
                try {
                    const msg = JSON.parse(line);
                    if (msg.QMP) {
                        socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                    } else if (msg.return !== undefined && !capabilitiesSent) {
                        capabilitiesSent = true;
                        socket.write(JSON.stringify({
                            "execute": "query-kernel-info",
                            "arguments": {"cpu-index": 0}
                        }) + '\n');
                    } else if (msg.return && msg.return.ttbr1 !== undefined) {
                        socket.end();
                        resolve(msg.return);
                    }
                } catch (e) {}
            }
        });

        socket.on('close', () => resolve(null));
        socket.on('error', () => resolve(null));
        socket.connect(4445, 'localhost');
    });
}

function translateVA(fd, va, swapperPgd) {
    if (va < 0xffff000000000000n) return 0;
    
    try {
        // Extract indices
        const pgdIndex = Number((va >> 39n) & 0x1FFn);
        const pudIndex = Number((va >> 30n) & 0x1FFn);
        const pmdIndex = Number((va >> 21n) & 0x1FFn);
        const pteIndex = Number((va >> 12n) & 0x1FFn);
        const pageOffset = Number(va & 0xFFFn);
        
        // Read PGD
        const pgdOffset = swapperPgd - GUEST_RAM_START;
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, pgdOffset);
        
        const pgdEntry = pgdBuffer.readBigUint64LE(pgdIndex * 8);
        if ((pgdEntry & 0x3n) === 0n) return 0;
        
        // Handle 1GB block
        if ((pgdEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pgdEntry & PA_MASK);
            return blockPA | (Number(va) & 0x3FFFFFFF);
        }
        
        // Read PUD
        const pudTablePA = Number(pgdEntry & PA_MASK);
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        
        const pudEntry = pudBuffer.readBigUint64LE(pudIndex * 8);
        if ((pudEntry & 0x3n) === 0n) return 0;
        
        // Handle 2MB block
        if ((pudEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pudEntry & PA_MASK);
            return blockPA | (Number(va) & 0x1FFFFF);
        }
        
        // Read PMD
        const pmdTablePA = Number(pudEntry & PA_MASK);
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        
        const pmdEntry = pmdBuffer.readBigUint64LE(pmdIndex * 8);
        if ((pmdEntry & 0x3n) === 0n) return 0;
        
        // Read PTE
        const pteTablePA = Number(pmdEntry & PA_MASK);
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        
        const pteEntry = pteBuffer.readBigUint64LE(pteIndex * 8);
        if ((pteEntry & 0x3n) === 0n) return 0;
        
        const pagePA = Number(pteEntry & PA_MASK);
        return pagePA | pageOffset;
        
    } catch (e) {
        return 0;
    }
}

function findInitTask(fd, fileSize) {
    console.log('Finding init_task (systemd, PID 1)...\n');
    
    // Based on our earlier discovery, systemd is at PA 0x100388000
    const knownLocations = [
        0x100388000,  // Where we found PID 1 before
        0x10038c700,  // Where we found PID 2 before
    ];
    
    for (const pa of knownLocations) {
        const offset = pa - GUEST_RAM_START;
        if (offset >= fileSize) continue;
        
        try {
            const pidBuffer = Buffer.allocUnsafe(4);
            fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
            const pid = pidBuffer.readUint32LE(0);
            
            if (pid === 1) {
                const commBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
                const comm = commBuffer.toString('ascii').split('\0')[0];
                
                if (comm === 'systemd' || comm === 'init') {
                    const tasksBuffer = Buffer.allocUnsafe(16);
                    fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
                    const tasksNext = tasksBuffer.readBigUint64LE(0);
                    const tasksPrev = tasksBuffer.readBigUint64LE(8);
                    
                    console.log(`Found init_task at PA 0x${pa.toString(16)}`);
                    console.log(`  PID: ${pid}, comm: "${comm}"`);
                    console.log(`  tasks.next: 0x${tasksNext.toString(16)}`);
                    console.log(`  tasks.prev: 0x${tasksPrev.toString(16)}\n`);
                    
                    return { pa, tasksNext, tasksPrev };
                }
            }
        } catch (e) {}
    }
    
    return null;
}

function walkCompleteTaskList(fd, initTask, swapperPgd) {
    console.log('Walking complete task list via init_task.tasks...\n');
    
    const processes = [];
    const visited = new Set();
    let errors = 0;
    
    // The init_task itself
    processes.push({ pid: 1, comm: 'systemd', pa: initTask.pa });
    
    // Start walking from init_task.tasks.next
    let currentVA = initTask.tasksNext;
    let count = 0;
    const maxIterations = 500;  // Safety limit

    // Check if tasksNext is valid
    if (!currentVA || currentVA === 0n) {
        console.log('ERROR: init_task.tasks.next is NULL or 0');
        console.log('This suggests init_task might not be the real init or list is corrupted');

        // Try using tasksPrev instead
        if (initTask.tasksPrev && initTask.tasksPrev !== 0n) {
            console.log(`Using tasks.prev instead: 0x${initTask.tasksPrev.toString(16)}`);
            currentVA = initTask.tasksPrev;
        } else {
            return processes;
        }
    }
    
    while (count < maxIterations) {
        // Convert tasks pointer to task_struct pointer
        const taskStructVA = currentVA - BigInt(TASKS_LIST_OFFSET);
        
        // Check for loop back to init_task
        const initTaskVA = BigInt(initTask.pa - GUEST_RAM_START) + 0xffff000000000000n;
        if (taskStructVA === initTaskVA) {
            console.log('Completed full circle back to init_task');
            break;
        }
        
        // Avoid infinite loops
        const key = taskStructVA.toString();
        if (visited.has(key)) {
            console.log('Loop detected (visited before)');
            break;
        }
        visited.add(key);
        
        // Translate VA to PA
        const taskStructPA = translateVA(fd, taskStructVA, swapperPgd);
        if (!taskStructPA) {
            errors++;
            if (errors <= 3) {
                console.log(`Failed to translate VA 0x${taskStructVA.toString(16)}`);
            }
            // Try to continue by reading the next pointer if we can
            break;
        }
        
        const taskOffset = taskStructPA - GUEST_RAM_START;
        
        try {
            // Read process info
            const pidBuffer = Buffer.allocUnsafe(4);
            fs.readSync(fd, pidBuffer, 0, 4, taskOffset + PID_OFFSET);
            const pid = pidBuffer.readUint32LE(0);
            
            const commBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, commBuffer, 0, 16, taskOffset + COMM_OFFSET);
            const comm = commBuffer.toString('ascii').split('\0')[0];
            
            // Validate
            if (pid > 0 && pid < 32768 && comm && comm.length > 0) {
                processes.push({ pid, comm, pa: taskStructPA });
                
                if (count % 20 === 0) {
                    console.log(`  Processed ${count + 1} tasks... (current: PID ${pid})`);
                }
            }
            
            // Read next pointer
            const tasksBuffer = Buffer.allocUnsafe(8);
            fs.readSync(fd, tasksBuffer, 0, 8, taskOffset + TASKS_LIST_OFFSET);
            const nextVA = tasksBuffer.readBigUint64LE(0);
            
            // Move to next
            currentVA = nextVA;
            count++;
            
        } catch (e) {
            errors++;
            if (errors <= 3) {
                console.log(`Error reading task at PA 0x${taskStructPA.toString(16)}: ${e.message}`);
            }
            break;
        }
    }
    
    if (count === maxIterations) {
        console.log('Reached maximum iteration limit');
    }
    
    return processes;
}

async function main() {
    console.log('=== Complete Process List Walk ===\n');
    
    const memoryPath = '/tmp/haywire-vm-mem';
    const fd = fs.openSync(memoryPath, 'r');
    const stats = fs.fstatSync(fd);
    const fileSize = stats.size;
    
    // Get swapper_pg_dir
    let swapperPgd = SWAPPER_PGD_PA;
    const kernelInfo = await getKernelInfo();
    if (kernelInfo) {
        swapperPgd = Number(BigInt(kernelInfo.ttbr1) & PA_MASK);
        console.log(`Using swapper_pg_dir from QMP: 0x${swapperPgd.toString(16)}\n`);
    } else {
        console.log(`Using known swapper_pg_dir: 0x${swapperPgd.toString(16)}\n`);
    }
    
    // Find init_task
    const initTask = findInitTask(fd, fileSize);
    
    if (!initTask) {
        console.log('Could not find init_task');
        fs.closeSync(fd);
        return;
    }
    
    // Walk the complete list
    const processes = walkCompleteTaskList(fd, initTask, swapperPgd);
    
    console.log('\n=== RESULTS ===\n');
    console.log(`Total processes found: ${processes.length}`);
    
    // Load ground truth for comparison
    try {
        const groundTruth = fs.readFileSync('ground_truth_processes.txt', 'utf-8')
            .split('\n')
            .filter(line => line.trim())
            .length;
        
        console.log(`Ground truth: ${groundTruth} processes`);
        console.log(`Discovery rate: ${((processes.length / groundTruth) * 100).toFixed(1)}%`);
    } catch (e) {
        console.log('No ground truth file for comparison');
    }
    
    // Show sample
    console.log('\nFirst 20 processes:');
    for (let i = 0; i < Math.min(20, processes.length); i++) {
        const p = processes[i];
        console.log(`  PID ${p.pid}: ${p.comm}`);
    }
    
    // Group by name
    const byName = new Map();
    for (const p of processes) {
        if (!byName.has(p.comm)) {
            byName.set(p.comm, 0);
        }
        byName.set(p.comm, byName.get(p.comm) + 1);
    }
    
    console.log('\nProcess types (top 10):');
    const sorted = Array.from(byName.entries()).sort((a, b) => b[1] - a[1]);
    for (const [name, count] of sorted.slice(0, 10)) {
        console.log(`  ${name}: ${count}`);
    }
    
    fs.closeSync(fd);
}

main().catch(console.error);