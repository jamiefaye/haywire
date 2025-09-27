#!/usr/bin/env node
/**
 * Direct test of open files discovery without UI
 * Run with: node test-open-files-direct.mjs
 */

import fs from 'fs';

// Memory reading class
class SimplePagedMemory {
    constructor(filePath) {
        this.filePath = filePath;
        this.fd = fs.openSync(filePath, 'r');
        this.stats = fs.fstatSync(this.fd);
        this.fileSize = this.stats.size;
    }

    readBytes(offset, size) {
        if (offset < 0 || offset >= this.fileSize) {
            return null;
        }

        const actualSize = Math.min(size, this.fileSize - offset);
        const buffer = Buffer.alloc(actualSize);

        try {
            fs.readSync(this.fd, buffer, 0, actualSize, offset);
            return new Uint8Array(buffer);
        } catch (e) {
            console.error(`Error reading at offset ${offset}:`, e);
            return null;
        }
    }

    close() {
        fs.closeSync(this.fd);
    }
}

// Offsets from pahole
const OFFSETS = {
    'task_struct.files': 0x9B8,     // 2488 - from pahole
    'task_struct.comm': 0x758,      // 1880 - process name
    'task_struct.pid': 0x4E8,       // 1256 - PID
    'task_struct.mm': 0x998,        // 2456 - mm_struct pointer
    'task_struct.tasks': 0x508,     // 1288 - task list
    'files_struct.fdt': 0x20,       // 32 - fdtable pointer
    'files_struct.count': 0x00,     // 0 - atomic count
    'fdtable.fd': 0x08,             // 8 - file descriptor array
    'fdtable.max_fds': 0x00,        // 0 - max fds
    'file.f_inode': 0x28,           // 40 - inode pointer
    'file.f_path': 0x10,            // 16 - file path
    'file.f_pos': 0x68,             // 104 - file position
    'inode.i_ino': 0x40,            // 64 - inode number
    'inode.i_size': 0x50,           // 80 - file size
    'inode.i_sb': 0x28,             // 40 - superblock pointer
    'inode.i_mapping': 0x30,        // 48 - address_space pointer
};

// Simple kernel VA translation (linear map only for now)
function translateKernelVA(va) {
    // Linear map: 0xffff800000000000 - 0xffff800080000000 -> PA 0x40000000+
    if (va >= 0xffff800000000000n && va < 0xffff800080000000n) {
        return Number(va - 0xffff800000000000n + 0x40000000n);
    }
    return null;
}

async function discoverOpenFiles() {
    console.log('=== Discovering Open Files Through Process File Tables ===\n');

    const memoryFile = '/tmp/haywire-vm-mem';

    if (!fs.existsSync(memoryFile)) {
        console.error(`Memory file not found: ${memoryFile}`);
        console.error('Make sure the VM is running with memory-backend-file');
        return;
    }

    console.log(`Opening memory file: ${memoryFile}`);
    const memory = new SimplePagedMemory(memoryFile);

    try {
        console.log(`File size: ${(memory.fileSize / (1024*1024*1024)).toFixed(2)} GB\n`);

        const openFiles = new Map(); // inode address -> file info

        // Start from init_task
        const initTaskVA = BigInt('0xffff8000838e2880'); // Known init_task address
        console.log(`Starting from init_task at VA 0x${initTaskVA.toString(16)}`);

        // Read first task to get the list head
        const tasksOffset = BigInt(OFFSETS['task_struct.tasks']);
        const firstTaskPA = translateKernelVA(initTaskVA + tasksOffset);

        if (!firstTaskPA) {
            console.error('Failed to translate init_task address');
            return;
        }

        const firstData = memory.readBytes(firstTaskPA - 0x40000000, 16);
        if (!firstData) {
            console.error('Failed to read init_task');
            return;
        }

        const dataView = new DataView(firstData.buffer, firstData.byteOffset, firstData.byteLength);
        let nextPtr = dataView.getBigUint64(0, true);
        const first = nextPtr;
        let taskCount = 0;
        const maxTasks = 200;
        const tasks = [];

        while (nextPtr && taskCount < maxTasks) {
            // Container_of: subtract task list offset to get task_struct address
            const taskAddr = nextPtr - tasksOffset;

            // Read PID
            const pidPA = translateKernelVA(taskAddr + BigInt(OFFSETS['task_struct.pid']));
            if (!pidPA) {
                break;
            }

            const pidData = memory.readBytes(pidPA - 0x40000000, 4);
            if (!pidData) {
                break;
            }

            const pid = new DataView(pidData.buffer, pidData.byteOffset, pidData.byteLength).getUint32(0, true);

            // Read comm (process name)
            const commPA = translateKernelVA(taskAddr + BigInt(OFFSETS['task_struct.comm']));
            let name = '';
            if (commPA) {
                const commData = memory.readBytes(commPA - 0x40000000, 16);
                if (commData) {
                    for (let i = 0; i < commData.length && commData[i] !== 0; i++) {
                        if (commData[i] >= 32 && commData[i] < 127) {
                            name += String.fromCharCode(commData[i]);
                        }
                    }
                }
            }

            // Only process user tasks (skip kernel threads)
            if (pid > 0 && name && !name.startsWith('[')) {
                tasks.push({addr: taskAddr, pid, name});

                // Get files_struct pointer
                const filesPA = translateKernelVA(taskAddr + BigInt(OFFSETS['task_struct.files']));
                if (filesPA) {
                    const filesData = memory.readBytes(filesPA - 0x40000000, 8);
                    if (filesData) {
                        const filesPtr = new DataView(filesData.buffer, filesData.byteOffset, filesData.byteLength).getBigUint64(0, true);

                        if (filesPtr && filesPtr > BigInt('0xffff000000000000')) {
                            // Read fdtable pointer
                            const fdtPA = translateKernelVA(filesPtr + BigInt(OFFSETS['files_struct.fdt']));
                            if (fdtPA) {
                                const fdtData = memory.readBytes(fdtPA - 0x40000000, 8);
                                if (fdtData) {
                                    const fdtPtr = new DataView(fdtData.buffer, fdtData.byteOffset, fdtData.byteLength).getBigUint64(0, true);

                                    if (fdtPtr && fdtPtr > BigInt('0xffff000000000000')) {
                                        // Read max_fds and fd array pointer
                                        const maxFdsPA = translateKernelVA(fdtPtr + BigInt(OFFSETS['fdtable.max_fds']));
                                        const fdArrayPA = translateKernelVA(fdtPtr + BigInt(OFFSETS['fdtable.fd']));

                                        if (maxFdsPA && fdArrayPA) {
                                            const maxFdsData = memory.readBytes(maxFdsPA - 0x40000000, 4);
                                            const fdArrayData = memory.readBytes(fdArrayPA - 0x40000000, 8);

                                            if (maxFdsData && fdArrayData) {
                                                const maxFds = new DataView(maxFdsData.buffer, maxFdsData.byteOffset, maxFdsData.byteLength).getUint32(0, true);
                                                const fdArrayPtr = new DataView(fdArrayData.buffer, fdArrayData.byteOffset, fdArrayData.byteLength).getBigUint64(0, true);

                                                // Check first few file descriptors
                                                const checkFds = Math.min(maxFds, 20);
                                                let foundFiles = 0;

                                                for (let fd = 0; fd < checkFds; fd++) {
                                                    const filePA = translateKernelVA(fdArrayPtr + BigInt(fd * 8));
                                                    if (!filePA) continue;

                                                    const fileData = memory.readBytes(filePA - 0x40000000, 8);
                                                    if (!fileData) continue;

                                                    const filePtr = new DataView(fileData.buffer, fileData.byteOffset, fileData.byteLength).getBigUint64(0, true);
                                                    if (filePtr && filePtr > BigInt('0xffff000000000000')) {
                                                        // Read inode pointer from file structure
                                                        const inodePA = translateKernelVA(filePtr + BigInt(OFFSETS['file.f_inode']));
                                                        if (!inodePA) continue;

                                                        const inodeData = memory.readBytes(inodePA - 0x40000000, 8);
                                                        if (!inodeData) continue;

                                                        const inodePtr = new DataView(inodeData.buffer, inodeData.byteOffset, inodeData.byteLength).getBigUint64(0, true);
                                                        if (inodePtr && inodePtr > BigInt('0xffff000000000000')) {
                                                            foundFiles++;

                                                            if (!openFiles.has(inodePtr)) {
                                                                // Read inode details
                                                                const inoPA = translateKernelVA(inodePtr + BigInt(OFFSETS['inode.i_ino']));
                                                                const sizePA = translateKernelVA(inodePtr + BigInt(OFFSETS['inode.i_size']));

                                                                let ino = BigInt(0);
                                                                let size = BigInt(0);

                                                                if (inoPA) {
                                                                    const inoData = memory.readBytes(inoPA - 0x40000000, 8);
                                                                    if (inoData) {
                                                                        ino = new DataView(inoData.buffer, inoData.byteOffset, inoData.byteLength).getBigUint64(0, true);
                                                                    }
                                                                }

                                                                if (sizePA) {
                                                                    const sizeData = memory.readBytes(sizePA - 0x40000000, 8);
                                                                    if (sizeData) {
                                                                        size = new DataView(sizeData.buffer, sizeData.byteOffset, sizeData.byteLength).getBigUint64(0, true);
                                                                    }
                                                                }

                                                                openFiles.set(inodePtr, {
                                                                    inodeAddr: inodePtr,
                                                                    ino: ino,
                                                                    size: size,
                                                                    processes: [{pid, name, fd}]
                                                                });
                                                            } else {
                                                                // Add this process to existing inode entry
                                                                openFiles.get(inodePtr).processes.push({pid, name, fd});
                                                            }
                                                        }
                                                    }
                                                }

                                                if (foundFiles > 0) {
                                                    console.log(`Process ${name} (PID ${pid}): found ${foundFiles} open files`);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            taskCount++;

            // Get next task
            const nextPA = translateKernelVA(nextPtr);
            if (!nextPA) break;

            const nextData = memory.readBytes(nextPA - 0x40000000, 8);
            if (!nextData) break;

            nextPtr = new DataView(nextData.buffer, nextData.byteOffset, nextData.byteLength).getBigUint64(0, true);

            // Check if we've looped back
            if (nextPtr === first) {
                console.log('Reached end of task list');
                break;
            }
        }

        console.log(`\nFound ${tasks.length} user processes`);
        console.log(`Discovered ${openFiles.size} unique open files\n`);

        // Show all discovered files
        if (openFiles.size > 0) {
            console.log('All discovered open files:\n');

            // Convert to array and sort by inode number
            const filesArray = Array.from(openFiles.values()).sort((a, b) => {
                if (a.ino < b.ino) return -1;
                if (a.ino > b.ino) return 1;
                return 0;
            });

            for (const info of filesArray) {
                console.log(`Inode #${info.ino}:`);
                console.log(`  Address: 0x${info.inodeAddr.toString(16)}`);
                console.log(`  Size: ${info.size} bytes`);
                console.log(`  Open by: ${info.processes.length} process(es)`);
                for (const proc of info.processes) {
                    console.log(`    - ${proc.name} (PID ${proc.pid}) on fd ${proc.fd}`);
                }
                console.log();
            }

            // Check if we found the known files
            const knownInodes = [272643, 262017, 267562]; // libpcre2-8.so, bash, systemctl
            console.log('Checking for known files (from /proc/meminfo cache list):');
            for (const ino of knownInodes) {
                const found = filesArray.find(f => f.ino === BigInt(ino));
                if (found) {
                    console.log(`  ✓ Found inode ${ino}`);
                } else {
                    console.log(`  ✗ Missing inode ${ino}`);
                }
            }
        }

    } finally {
        memory.close();
    }
}

// Run the test
discoverOpenFiles().catch(console.error);