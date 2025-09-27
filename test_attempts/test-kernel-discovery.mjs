#!/usr/bin/env node
/**
 * Standalone kernel discovery test
 * Run with: node test-kernel-discovery.mjs /path/to/memory.bin
 */

import fs from 'fs';

// Kernel constants from kernel-discovery-paged.ts
const KernelConstants = {
    TASK_STRUCT_SIZE: 9088,
    PID_OFFSET: 0x750,
    COMM_OFFSET: 0x970,
    MM_OFFSET: 0x6d0,
    PGD_OFFSET_IN_MM: 0x68,
    GUEST_RAM_START: 0x40000000,
    PA_MASK: 0x0000FFFFFFFFF000n,
    SLAB_OFFSETS: [0x0, 0x2380, 0x4700],
};

// PagedMemory implementation
class PagedMemory {
    constructor(fd, fileSize, chunkSize = 100 * 1024 * 1024) {
        this.fd = fd;
        this.fileSize = fileSize;
        this.chunkSize = chunkSize;
        this.cache = new Map();
        this.maxCachedChunks = 10;
    }

    getChunkIndex(offset) {
        return Math.floor(offset / this.chunkSize);
    }

    loadChunk(chunkIndex) {
        if (this.cache.has(chunkIndex)) {
            return this.cache.get(chunkIndex);
        }

        const start = chunkIndex * this.chunkSize;
        const size = Math.min(this.chunkSize, this.fileSize - start);
        const buffer = Buffer.allocUnsafe(size);

        fs.readSync(this.fd, buffer, 0, size, start);

        if (this.cache.size >= this.maxCachedChunks) {
            const firstKey = this.cache.keys().next().value;
            this.cache.delete(firstKey);
        }

        this.cache.set(chunkIndex, buffer);
        return buffer;
    }

    readU32(offset) {
        if (offset + 4 > this.fileSize) return null;

        const chunkIndex = this.getChunkIndex(offset);
        const chunk = this.loadChunk(chunkIndex);
        const localOffset = offset - (chunkIndex * this.chunkSize);

        if (localOffset + 4 > chunk.length) {
            const bytes = Buffer.allocUnsafe(4);
            for (let i = 0; i < 4; i++) {
                bytes[i] = this.readByte(offset + i);
            }
            return bytes.readUInt32LE(0);
        }

        return chunk.readUInt32LE(localOffset);
    }

    readU64(offset) {
        if (offset + 8 > this.fileSize) return null;

        const chunkIndex = this.getChunkIndex(offset);
        const chunk = this.loadChunk(chunkIndex);
        const localOffset = offset - (chunkIndex * this.chunkSize);

        if (localOffset + 8 > chunk.length) {
            const bytes = Buffer.allocUnsafe(8);
            for (let i = 0; i < 8; i++) {
                bytes[i] = this.readByte(offset + i);
            }
            return bytes.readBigUint64LE(0);
        }

        return chunk.readBigUint64LE(localOffset);
    }

    readByte(offset) {
        if (offset >= this.fileSize) return 0;

        const chunkIndex = this.getChunkIndex(offset);
        const chunk = this.loadChunk(chunkIndex);
        const localOffset = offset - (chunkIndex * this.chunkSize);

        return chunk[localOffset];
    }

    readString(offset, maxLen = 16) {
        let str = '';
        for (let i = 0; i < maxLen; i++) {
            const byte = this.readByte(offset + i);
            if (byte === 0) break;
            if (byte >= 32 && byte <= 126) {
                str += String.fromCharCode(byte);
            } else {
                return null; // Non-printable character
            }
        }
        return str;
    }

    getTotalSize() {
        return this.fileSize;
    }
}

// Main kernel discovery class
class KernelDiscovery {
    constructor(memory) {
        this.memory = memory;
    }

    findProcesses() {
        console.log('=== SCANNING FOR PROCESSES ===\n');
        const processes = [];
        const fileSize = this.memory.getTotalSize();
        const pageSize = 4096;

        // Scan first 200MB for task_structs
        const maxScan = Math.min(200 * 1024 * 1024, fileSize);

        for (let offset = 0; offset < maxScan; offset += pageSize) {
            for (const slabOffset of KernelConstants.SLAB_OFFSETS) {
                const taskOffset = offset + slabOffset;
                if (taskOffset + KernelConstants.TASK_STRUCT_SIZE > fileSize) continue;

                const pid = this.memory.readU32(taskOffset + KernelConstants.PID_OFFSET);
                if (pid === null || pid === 0 || pid > 100000) continue;

                const comm = this.memory.readString(taskOffset + KernelConstants.COMM_OFFSET);
                if (!comm || comm.length === 0) continue;

                // Read mm_struct pointer
                const mmPtr = this.memory.readU64(taskOffset + KernelConstants.MM_OFFSET);

                processes.push({
                    pid: pid,
                    name: comm,
                    taskAddr: taskOffset + KernelConstants.GUEST_RAM_START,
                    mmPtr: mmPtr
                });

                if (processes.length % 10 === 0) {
                    process.stdout.write(`Found ${processes.length} processes...\r`);
                }
            }
        }

        console.log(`\nFound ${processes.length} total processes\n`);
        return processes;
    }

    extractPGDs(processes) {
        console.log('=== EXTRACTING PGDS ===\n');
        let validPgdCount = 0;
        let invalidPgdCount = 0;
        let nullMmCount = 0;

        for (const process of processes) {
            if (!process.mmPtr || process.mmPtr === 0n) {
                nullMmCount++;
                if (nullMmCount <= 5) {
                    console.log(`PID ${process.pid} (${process.name}): mm_struct is NULL (kernel thread)`);
                }
                continue;
            }

            // Calculate file offset for mm_struct
            const mmOffset = Number(process.mmPtr) - KernelConstants.GUEST_RAM_START;

            if (mmOffset < 0 || mmOffset >= this.memory.getTotalSize()) {
                invalidPgdCount++;
                if (invalidPgdCount <= 5) {
                    console.log(`PID ${process.pid} (${process.name}): mm_struct 0x${process.mmPtr.toString(16)} outside file range`);
                }
                continue;
            }

            // Read PGD from mm_struct
            const pgdRaw = this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM);

            if (!pgdRaw || pgdRaw === 0n) {
                invalidPgdCount++;
                if (invalidPgdCount <= 5) {
                    console.log(`PID ${process.pid} (${process.name}): PGD is NULL`);
                }
                continue;
            }

            // Apply PA_MASK
            const pgdPA = pgdRaw & KernelConstants.PA_MASK;

            // Check if PGD looks valid
            if (pgdPA > 0x1000n && pgdPA < 0x200000000n) {
                validPgdCount++;
                process.pgd = pgdPA;

                if (validPgdCount <= 10) {
                    console.log(`PID ${process.pid} (${process.name}): PGD = 0x${pgdPA.toString(16)}`);
                }
            } else {
                invalidPgdCount++;
                if (invalidPgdCount <= 5) {
                    console.log(`PID ${process.pid} (${process.name}): Invalid PGD 0x${pgdRaw.toString(16)}`);
                }
            }
        }

        console.log(`\nPGD Summary:`);
        console.log(`  Valid PGDs: ${validPgdCount}`);
        console.log(`  Invalid PGDs: ${invalidPgdCount}`);
        console.log(`  NULL mm_structs: ${nullMmCount}`);

        return processes.filter(p => p.pgd);
    }

    walkPageTable(pgdPA) {
        const ptes = [];
        const pgdOffset = Number(pgdPA) - KernelConstants.GUEST_RAM_START;

        if (pgdOffset < 0 || pgdOffset >= this.memory.getTotalSize()) {
            return ptes;
        }

        // Walk PGD entries (simplified - just check first few)
        for (let i = 0; i < 5; i++) {
            const entry = this.memory.readU64(pgdOffset + i * 8);
            if (!entry || entry === 0n) continue;

            const entryType = Number(entry) & 0x3;
            const physAddr = entry & KernelConstants.PA_MASK;

            if (entryType === 0x3) {
                // Table entry - would recurse to PUD/PMD/PTE
                ptes.push({
                    va: BigInt(i) << 39n,  // Simplified VA calculation
                    pa: physAddr,
                    flags: Number(entry) & 0xFFF
                });
            }
        }

        return ptes;
    }

    async run() {
        // Find all processes
        const processes = this.findProcesses();

        // Extract PGDs
        const processesWithPGD = this.extractPGDs(processes);

        // Walk page tables for a few processes
        console.log('\n=== WALKING PAGE TABLES ===\n');
        let pteCount = 0;

        for (const process of processesWithPGD.slice(0, 10)) {
            console.log(`\nWalking PGD for PID ${process.pid} (${process.name}):`);
            const ptes = this.walkPageTable(process.pgd);

            if (ptes.length > 0) {
                console.log(`  Found ${ptes.length} PTEs`);
                pteCount += ptes.length;

                for (const pte of ptes.slice(0, 3)) {
                    console.log(`    VA: 0x${pte.va.toString(16)} -> PA: 0x${pte.pa.toString(16)}`);
                }
            } else {
                console.log(`  No PTEs found`);
            }
        }

        console.log(`\n=== SUMMARY ===`);
        console.log(`Total processes found: ${processes.length}`);
        console.log(`Processes with valid PGDs: ${processesWithPGD.length}`);
        console.log(`Total PTEs found: ${pteCount}`);
    }
}

async function main() {
    const filePath = process.argv[2];
    if (!filePath) {
        console.error('Usage: node test-kernel-discovery.mjs /path/to/memory.bin');
        process.exit(1);
    }

    console.log('==================================================');
    console.log('        KERNEL DISCOVERY TEST                    ');
    console.log('==================================================');
    console.log(`File: ${filePath}`);

    const stats = fs.statSync(filePath);
    const fd = fs.openSync(filePath, 'r');
    const memory = new PagedMemory(fd, stats.size);
    console.log(`Size: ${(stats.size / (1024*1024*1024)).toFixed(2)} GB\n`);

    const discovery = new KernelDiscovery(memory);
    await discovery.run();

    fs.closeSync(fd);
}

main().catch(console.error);