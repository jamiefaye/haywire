#!/usr/bin/env node
/**
 * Test open files discovery with proper kernel page table translation
 * This version uses the kernel discovery to properly translate virtual addresses
 */

import fs from 'fs';
import { execSync } from 'child_process';

// Import the TypeScript modules directly using Vite
async function runTest() {
    const { PagedMemory } = await import('./src/paged-memory.ts');
    const { KernelMem } = await import('./src/kernel-mem.ts');
    const { PagedKernelDiscovery } = await import('./src/kernel-discovery-paged.ts');

    console.log('\n=== Testing Open Files Discovery with Kernel Translation ===\n');

    // Open the memory file
    const memoryFile = '/Users/jamie/haywire/haywire-vm-mem';
    console.log(`Opening memory file: ${memoryFile}`);

    // Read file in chunks to avoid 2GB limit
    const stats = fs.statSync(memoryFile);
    const fileSize = stats.size;
    const chunkSize = 100 * 1024 * 1024; // 100MB chunks
    const totalChunks = Math.ceil(fileSize / chunkSize);

    console.log(`Reading ${(fileSize / (1024*1024*1024)).toFixed(1)} GB file in ${totalChunks} chunks...`);

    const chunks = [];
    const fd = fs.openSync(memoryFile, 'r');

    for (let i = 0; i < totalChunks; i++) {
        const offset = i * chunkSize;
        const size = Math.min(chunkSize, fileSize - offset);
        const buffer = Buffer.alloc(size);
        fs.readSync(fd, buffer, 0, size, offset);
        chunks.push(new Uint8Array(buffer));
        process.stdout.write(`\rReading chunk ${i + 1}/${totalChunks}...`);
    }
    fs.closeSync(fd);
    console.log(' Done!');

    // Load chunks into PagedMemory
    const memory = await PagedMemory.fromChunks(chunks);
    console.log(`PagedMemory loaded: ${memory.getMemoryUsage()}`);

    console.log(`Loaded memory file: ${(fileSize / (1024*1024*1024)).toFixed(2)} GB`);

    // Create kernel discovery - this handles all the page table translation
    const kernelDiscovery = new PagedKernelDiscovery(memory);

    // Run kernel discovery to find processes and PGD
    console.log('\nRunning kernel discovery...');
    const discoveryResults = await kernelDiscovery.discover(fileSize);

    if (!discoveryResults.swapperPgDir) {
        console.error('Failed to find kernel PGD - cannot translate kernel VAs');
        return;
    }

    console.log(`✓ Kernel PGD found at PA: 0x${discoveryResults.swapperPgDir.toString(16)}`);
    console.log(`  Found ${discoveryResults.processes?.length || 0} processes`);

    // Create KernelMem instance and get ground truth PGD
    const kmem = new KernelMem(memory);

    // First try to get ground truth from QMP
    console.log('\n=== Querying Ground Truth PGD from KernelMem Library ===\n');
    const groundTruthPgd = await kmem.queryGroundTruthPgd();

    if (groundTruthPgd) {
        console.log(`✓ Using QMP ground truth PGD: 0x${groundTruthPgd.toString(16)}`);
    } else {
        console.log(`⚠ No QMP ground truth available, using discovered PGD: 0x${discoveryResults.swapperPgDir.toString(16)}`);
        kmem.setKernelPgd(discoveryResults.swapperPgDir);
    }

    // Use the library's findProcesses method
    console.log('\n=== Finding Processes with KernelMem Library ===\n');
    const processMap = kmem.findProcesses();
    console.log(`Found ${processMap.size} processes via KernelMem library`);

    // Call the open files discovery method directly
    console.log('\n=== Discovering Open Files Through Process File Tables ===\n');
    const openFiles = new Map();

    for (const [pid, proc] of processMap) {
        if (!proc.files || proc.files === 0n) {
            console.log(`  ${proc.name}[${proc.pid}]: No files pointer`);
            continue;
        }

        console.log(`  ${proc.name}[${proc.pid}]: files=0x${proc.files.toString(16)}`);

        // Read process file table using KernelMem - files is a kernel VA
        const fdtPtr = kmem.readU64(proc.files + 0x20n); // files.fdt offset at 0x20, not 0x08
        if (!fdtPtr) {
            console.log(`    Could not read fdt pointer at 0x${(proc.files + 0x20n).toString(16)}`);
            continue;
        }
        console.log(`    fdt=0x${fdtPtr.toString(16)}`);

        const maxFds = kmem.readU32(fdtPtr + 0x00n); // fdt.max_fds offset
        const fdArrayPtr = kmem.readU64(fdtPtr + 0x08n); // fdt.fd offset
        if (!maxFds || !fdArrayPtr) continue;

        const checkFds = Math.min(maxFds, 100);
        let foundInProc = 0;

        for (let fd = 0; fd < checkFds; fd++) {
            const filePtr = kmem.readU64(fdArrayPtr + BigInt(fd * 8));
            if (!filePtr || filePtr === 0n) continue;

            const inodePtr = kmem.readU64(filePtr + 0x28n); // file.inode offset
            if (!inodePtr || inodePtr === 0n) continue;

            foundInProc++;

            if (!openFiles.has(inodePtr)) {
                const ino = kmem.readU64(inodePtr + 0x40n) || 0n; // inode.ino offset
                const size = kmem.readU64(inodePtr + 0x50n) || 0n; // inode.size offset
                const mode = kmem.readU32(inodePtr + 0x00n) || 0; // inode.mode offset

                if (ino > 0n || size > 0n || mode > 0) {
                    openFiles.set(inodePtr, {
                        inodeAddr: inodePtr,
                        ino: ino.toString(),
                        size: size,
                        mode: mode,
                        processes: [{ name: proc.name, pid: proc.pid, fd: fd }]
                    });
                }
            } else {
                openFiles.get(inodePtr).processes.push({ name: proc.name, pid: proc.pid, fd: fd });
            }
        }

        if (foundInProc > 0) {
            console.log(`  ${proc.name}[${proc.pid}]: ${foundInProc} open files`);
        }
    }

    console.log(`\n=== Results ===`);
    console.log(`Found ${openFiles.size} unique open files\n`);

    if (openFiles.size > 0) {
        // Convert to array and sort by inode number
        const filesArray = Array.from(openFiles.values()).sort((a, b) => {
            const aNum = Number(a.ino);
            const bNum = Number(b.ino);
            return aNum - bNum;
        });

        console.log('All discovered open files:\n');
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

        // Check for known files from /proc/meminfo
        const knownInodes = [272643, 262017, 267562]; // libpcre2-8.so, bash, systemctl
        console.log('\nChecking for known cached files (from /proc/meminfo):');
        for (const ino of knownInodes) {
            const found = filesArray.find(f => Number(f.ino) === ino);
            if (found) {
                console.log(`  ✓ Found inode ${ino} - opened by: ${found.processes.map(p => p.name).join(', ')}`);
            } else {
                console.log(`  ✗ Missing inode ${ino}`);
            }
        }

        // Show statistics
        const stats = {
            totalFiles: openFiles.size,
            totalSize: filesArray.reduce((sum, f) => sum + Number(f.size), 0),
            byProcess: {}
        };

        for (const file of filesArray) {
            for (const proc of file.processes) {
                if (!stats.byProcess[proc.name]) {
                    stats.byProcess[proc.name] = { count: 0, fds: [] };
                }
                stats.byProcess[proc.name].count++;
                stats.byProcess[proc.name].fds.push(proc.fd);
            }
        }

        console.log('\n=== Statistics ===');
        console.log(`Total unique files: ${stats.totalFiles}`);
        console.log(`Total size: ${(stats.totalSize / 1024).toFixed(2)} KB`);
        console.log('\nFiles per process:');
        for (const [name, data] of Object.entries(stats.byProcess)) {
            console.log(`  ${name}: ${data.count} files`);
        }
    } else {
        console.log('No open files discovered - this might indicate:');
        console.log('  1. Wrong offsets for task_struct/files_struct/etc');
        console.log('  2. init_task address is incorrect');
        console.log('  3. Kernel page table translation is failing');
    }
}

// Run the test
runTest().catch(console.error);