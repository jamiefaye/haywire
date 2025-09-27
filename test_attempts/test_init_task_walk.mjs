#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

// Constants
const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

// Known offsets for ARM64 Linux 6.x
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const MM_OFFSET = 0x6d0;
const TASKS_LIST_OFFSET = 0x7e0;  // task_struct.tasks list_head

// Known swapper_pg_dir from our testing
const SWAPPER_PGD_PA = 0x136deb000;

async function getKernelInfo() {
    return new Promise((resolve, reject) => {
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

// Simplified VA translation using swapper_pg_dir
function translateVA(fd, va, swapperPgd) {
    // For kernel addresses (0xffff...)
    if (va < 0xffff000000000000n) {
        console.log(`  VA 0x${va.toString(16)} is not a kernel address`);
        return 0;
    }

    // Extract indices from VA
    const pgdIndex = Number((va >> 39n) & 0x1FFn);
    const pudIndex = Number((va >> 30n) & 0x1FFn);
    const pmdIndex = Number((va >> 21n) & 0x1FFn);
    const pteIndex = Number((va >> 12n) & 0x1FFn);
    const pageOffset = Number(va & 0xFFFn);

    console.log(`  Translating VA 0x${va.toString(16)}`);
    console.log(`    Indices: PGD[${pgdIndex}] PUD[${pudIndex}] PMD[${pmdIndex}] PTE[${pteIndex}] +0x${pageOffset.toString(16)}`);

    // Read PGD entry
    const pgdOffset = swapperPgd - GUEST_RAM_START;
    const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, pgdOffset);

    const pgdEntry = pgdBuffer.readBigUint64LE(pgdIndex * 8);
    if ((pgdEntry & 0x3n) === 0n) {
        console.log(`    PGD[${pgdIndex}] is invalid`);
        return 0;
    }

    // Handle block at PGD level (1GB huge page)
    if ((pgdEntry & 0x3n) === 0x1n) {
        const blockPA = Number(pgdEntry & PA_MASK);
        const pa = blockPA | (Number(va) & 0x3FFFFFFF);  // 1GB block
        console.log(`    PGD[${pgdIndex}] is 1GB block -> PA 0x${pa.toString(16)}`);
        return pa;
    }

    // Walk to PUD
    const pudTablePA = Number(pgdEntry & PA_MASK);
    const pudOffset = pudTablePA - GUEST_RAM_START;
    const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudOffset);

    const pudEntry = pudBuffer.readBigUint64LE(pudIndex * 8);
    if ((pudEntry & 0x3n) === 0n) {
        console.log(`    PUD[${pudIndex}] is invalid`);
        return 0;
    }

    // Handle block at PUD level (2MB huge page)
    if ((pudEntry & 0x3n) === 0x1n) {
        const blockPA = Number(pudEntry & PA_MASK);
        const pa = blockPA | (Number(va) & 0x1FFFFF);  // 2MB block
        console.log(`    PUD[${pudIndex}] is 2MB block -> PA 0x${pa.toString(16)}`);
        return pa;
    }

    // Walk to PMD
    const pmdTablePA = Number(pudEntry & PA_MASK);
    const pmdOffset = pmdTablePA - GUEST_RAM_START;
    const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, pmdOffset);

    const pmdEntry = pmdBuffer.readBigUint64LE(pmdIndex * 8);
    if ((pmdEntry & 0x3n) === 0n) {
        console.log(`    PMD[${pmdIndex}] is invalid`);
        return 0;
    }

    // Walk to PTE
    const pteTablePA = Number(pmdEntry & PA_MASK);
    const pteOffset = pteTablePA - GUEST_RAM_START;
    const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pteBuffer, 0, PAGE_SIZE, pteOffset);

    const pteEntry = pteBuffer.readBigUint64LE(pteIndex * 8);
    if ((pteEntry & 0x3n) === 0n) {
        console.log(`    PTE[${pteIndex}] is invalid`);
        return 0;
    }

    // Get final page
    const pagePA = Number(pteEntry & PA_MASK);
    const finalPA = pagePA | pageOffset;
    console.log(`    Final PA: 0x${finalPA.toString(16)}`);

    return finalPA;
}

// Find init_task by scanning for PID 0 with "swapper" name
function findInitTask(fd, fileSize) {
    console.log('Searching for init_task (PID 0, name "swapper")...\n');

    const searchRegions = [
        [0x100000000, 0x101000000], // 4GB mark (where we found it)
        [0x40000000, 0x42000000],   // First 32MB of kernel
        [0x80000000, 0x82000000],   // Alternative location
    ];

    for (const [regionStart, regionEnd] of searchRegions) {
        const start = Math.max(0, regionStart - GUEST_RAM_START);
        const end = Math.min(fileSize, regionEnd - GUEST_RAM_START);

        console.log(`Scanning region 0x${regionStart.toString(16)}-0x${regionEnd.toString(16)}...`);

        for (let offset = start; offset < end; offset += 8) {
            // Try to read PID
            const pidBuffer = Buffer.allocUnsafe(4);
            try {
                fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
            } catch {
                continue;
            }

            const pid = pidBuffer.readUint32LE(0);
            if (pid !== 0) continue;

            // Check comm field
            const commBuffer = Buffer.allocUnsafe(16);
            try {
                fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
            } catch {
                continue;
            }

            const comm = commBuffer.toString('ascii').split('\0')[0];
            if (comm === 'swapper' || comm === 'swapper/0') {
                const taskStructPA = offset + GUEST_RAM_START;
                console.log(`  Found potential init_task at PA 0x${taskStructPA.toString(16)}`);
                console.log(`    PID: ${pid}, comm: "${comm}"`);

                // Verify it looks like a valid task_struct
                const tasksBuffer = Buffer.allocUnsafe(16);
                try {
                    fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
                    const tasksNext = tasksBuffer.readBigUint64LE(0);
                    const tasksPrev = tasksBuffer.readBigUint64LE(8);

                    console.log(`    tasks.next: 0x${tasksNext.toString(16)}`);
                    console.log(`    tasks.prev: 0x${tasksPrev.toString(16)}`);

                    // Kernel addresses should be 0xffff...
                    if (tasksNext >= 0xffff000000000000n && tasksPrev >= 0xffff000000000000n) {
                        console.log(`    ✓ Looks like valid init_task!\n`);
                        return {
                            pa: taskStructPA,
                            tasksNext,
                            tasksPrev
                        };
                    }
                } catch {}
            }
        }
    }

    console.log('init_task not found by signature\n');
    return null;
}

// Walk the process list from init_task
function walkProcessList(fd, initTask, swapperPgd) {
    console.log('Walking process list from init_task...\n');

    const processes = [];
    const visited = new Set();

    // Start from init_task
    let currentVA = initTask.tasksNext;
    let count = 0;
    const maxProcesses = 20;  // Limit for testing

    while (count < maxProcesses) {
        // tasks.next points to the next tasks list_head, not the start of task_struct
        // We need to subtract TASKS_LIST_OFFSET to get to the start of task_struct
        const taskStructVA = currentVA - BigInt(TASKS_LIST_OFFSET);

        // Avoid infinite loops
        if (visited.has(taskStructVA.toString())) {
            console.log('Loop detected, stopping walk');
            break;
        }
        visited.add(taskStructVA.toString());

        console.log(`\nProcess ${count + 1}:`);
        console.log(`  tasks.next VA: 0x${currentVA.toString(16)}`);
        console.log(`  task_struct VA: 0x${taskStructVA.toString(16)}`);

        // Translate to physical address
        const taskStructPA = translateVA(fd, taskStructVA, swapperPgd);
        if (!taskStructPA) {
            console.log('  Failed to translate task_struct VA');
            break;
        }

        // Read process info
        const taskOffset = taskStructPA - GUEST_RAM_START;

        // Read PID
        const pidBuffer = Buffer.allocUnsafe(4);
        try {
            fs.readSync(fd, pidBuffer, 0, 4, taskOffset + PID_OFFSET);
            const pid = pidBuffer.readUint32LE(0);

            // Read comm
            const commBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, commBuffer, 0, 16, taskOffset + COMM_OFFSET);
            const comm = commBuffer.toString('ascii').split('\0')[0];

            console.log(`  ✓ Found: PID ${pid}, name="${comm}"`);
            processes.push({ pid, comm });

            // Read next pointer
            const tasksBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, tasksBuffer, 0, 16, taskOffset + TASKS_LIST_OFFSET);
            const nextVA = tasksBuffer.readBigUint64LE(0);

            // Check if we've looped back to init_task
            if (nextVA === initTask.tasksNext) {
                console.log('\nReached end of list (back to init_task)');
                break;
            }

            currentVA = nextVA;
            count++;

        } catch (e) {
            console.log(`  Error reading process data: ${e.message}`);
            break;
        }
    }

    return processes;
}

async function main() {
    console.log('=== Init Task Process List Walk Test ===\n');

    const memoryPath = '/tmp/haywire-vm-mem';
    const fd = fs.openSync(memoryPath, 'r');
    const stats = fs.fstatSync(fd);
    const fileSize = stats.size;
    console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB`);

    // Get swapper_pg_dir from QMP if available
    let swapperPgd = SWAPPER_PGD_PA;
    const kernelInfo = await getKernelInfo();
    if (kernelInfo) {
        swapperPgd = Number(BigInt(kernelInfo.ttbr1) & PA_MASK);
        console.log(`Got swapper_pg_dir from QMP: 0x${swapperPgd.toString(16)}\n`);
    } else {
        console.log(`Using known swapper_pg_dir: 0x${swapperPgd.toString(16)}\n`);
    }

    // Find init_task
    const initTask = findInitTask(fd, fileSize);

    if (initTask) {
        // Walk the process list
        const processes = walkProcessList(fd, initTask, swapperPgd);

        console.log('\n=== SUMMARY ===');
        console.log(`Found ${processes.length} processes:`);
        for (const proc of processes) {
            console.log(`  PID ${proc.pid}: ${proc.comm}`);
        }
    } else {
        console.log('Could not find init_task - cannot walk process list');
    }

    fs.closeSync(fd);
}

main().catch(console.error);