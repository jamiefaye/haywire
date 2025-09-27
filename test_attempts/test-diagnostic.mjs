#!/usr/bin/env node
/**
 * Standalone kernel diagnostic test
 * Run with: node test-diagnostic.mjs /path/to/memory.bin
 */

import fs from 'fs';

// Kernel constants from CURRENT kernel-discovery.ts (what found 122 processes)
const KernelConstants = {
    TASK_STRUCT_SIZE: 9088,
    PID_OFFSET: 0x750,      // Current working offset
    COMM_OFFSET: 0x970,     // Current working offset
    MM_OFFSET: 0x6d0,       // 1744 decimal - current working offset
    PGD_OFFSET_IN_MM: 0x68, // 104 decimal - from CLAUDE.md
    GUEST_RAM_START: 0x40000000,
    PA_MASK: 0x0000FFFFFFFFF000n,
    SLAB_OFFSETS: [0x0, 0x2380, 0x4700], // Where task_structs are found
};

// Simplified PagedMemory for large files
class PagedMemory {
    constructor(fd, fileSize, chunkSize = 100 * 1024 * 1024) { // 100MB chunks
        this.fd = fd;
        this.fileSize = fileSize;
        this.chunkSize = chunkSize;
        this.cache = new Map(); // Cache loaded chunks
        this.maxCachedChunks = 10; // Keep 10 chunks in memory (1GB)
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

        // Manage cache size
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
            // Spans chunks - read bytes individually
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
            // Spans chunks - read bytes individually
            const bytes = Buffer.allocUnsafe(8);
            for (let i = 0; i < 8; i++) {
                bytes[i] = this.readByte(offset + i);
            }
            return bytes.readBigUInt64LE(0);
        }

        return chunk.readBigUInt64LE(localOffset);
    }

    readByte(offset) {
        if (offset >= this.fileSize) return 0;

        const chunkIndex = this.getChunkIndex(offset);
        const chunk = this.loadChunk(chunkIndex);
        const localOffset = offset - (chunkIndex * this.chunkSize);

        return chunk[localOffset];
    }

    getTotalSize() {
        return this.fileSize;
    }
}

async function diagnoseProcess(memory, pid) {
    console.log(`\n=== DIAGNOSING PID ${pid} ===`);

    // Find task_struct
    const taskStruct = findTaskStructByPid(memory, pid);
    if (!taskStruct) {
        console.log(`   ❌ Could not find task_struct for PID ${pid}`);
        return;
    }

    console.log(`   ✓ Found task_struct at 0x${taskStruct.toString(16)}`);

    // Read mm_struct pointer from task_struct
    const mmOffset = taskStruct - KernelConstants.GUEST_RAM_START + KernelConstants.MM_OFFSET;
    console.log(`   Reading mm_struct from offset 0x${mmOffset.toString(16)}`);
    const mmPtrRaw = memory.readU64(mmOffset);

    if (!mmPtrRaw || mmPtrRaw === 0n) {
        console.log(`   ❌ mm_struct is NULL (kernel thread?)`);
        return;
    }

    // Convert BigInt to Number for mmPtr
    const mmPtr = Number(mmPtrRaw);
    console.log(`   ✓ mm_struct pointer: 0x${mmPtr.toString(16)}`);

    // Kernel pointers should look like 0xffff... for kernel space
    // But mm_struct might be a physical address in our case
    if (mmPtr < 0x1000) {
        console.log(`   ❌ Invalid mm_struct pointer (too small)`);
        return;
    }

    // Read PGD from mm_struct
    // Calculate file offset for mm_struct
    const mmFileOffset = mmPtr - KernelConstants.GUEST_RAM_START;
    console.log(`   mm_struct file offset: 0x${mmFileOffset.toString(16)}`);

    if (mmFileOffset < 0 || mmFileOffset >= memory.getTotalSize()) {
        console.log(`   ❌ mm_struct pointer outside memory range`);
        return;
    }

    // Read PGD pointer from mm_struct
    const pgdRaw = memory.readU64(mmFileOffset + KernelConstants.PGD_OFFSET_IN_MM);

    if (!pgdRaw) {
        console.log(`   ❌ Could not read PGD from mm_struct`);
        return;
    }

    console.log(`   Raw PGD value: 0x${pgdRaw.toString(16)}`);

    const pgdPA = pgdRaw & 0x0000FFFFFFFFF000n;
    console.log(`   Physical PGD: 0x${pgdPA.toString(16)}`);

    // Dump PGD entries
    console.log('\n   PGD ENTRIES:');
    dumpPageTableEntries(memory, Number(pgdPA), 'PGD', 5);
}

function findTaskStructByPid(memory, targetPid) {
    const fileSize = memory.getTotalSize();
    const pageSize = 4096;

    for (let offset = 0; offset < fileSize; offset += pageSize) {
        for (const slabOffset of KernelConstants.SLAB_OFFSETS) {
            const taskOffset = offset + slabOffset;
            if (taskOffset + 9088 > fileSize) continue; // TASK_STRUCT_SIZE

            const pidValue = memory.readU32(taskOffset + KernelConstants.PID_OFFSET);
            if (pidValue === targetPid) {
                return taskOffset + 0x40000000;
            }
        }
    }
    return null;
}

function dumpPageTableEntries(memory, tableAddr, level, count) {
    const tableOffset = tableAddr - 0x40000000;

    let validCount = 0;
    for (let i = 0; i < count; i++) {
        const entry = memory.readU64(tableOffset + i * 8);
        if (!entry || entry === 0n) continue;

        validCount++;
        const entryType = Number(entry) & 0x3;
        const physAddr = entry & 0x0000FFFFFFFFF000n;

        console.log(`     [${i}]: 0x${entry.toString(16)} -> PA: 0x${physAddr.toString(16)} (type: ${entryType})`);
    }

    if (validCount === 0) {
        console.log(`     ❌ No valid entries found`);
    }
}

async function main() {
    const filePath = process.argv[2];
    if (!filePath) {
        console.error('Usage: node test-diagnostic.mjs /path/to/memory.bin');
        process.exit(1);
    }

    console.log('==================================================');
    console.log('        KERNEL STRUCTURE DIAGNOSTIC              ');
    console.log('==================================================');
    console.log(`File: ${filePath}`);

    // Open file and create PagedMemory
    const stats = fs.statSync(filePath);
    const fd = fs.openSync(filePath, 'r');
    const memory = new PagedMemory(fd, stats.size);
    console.log(`Size: ${(stats.size / (1024*1024*1024)).toFixed(2)} GB`);

    // Test PIDs
    await diagnoseProcess(memory, 1);  // init/systemd
    await diagnoseProcess(memory, 2);  // kthreadd

    // Let's scan for ANY valid-looking task_structs
    console.log('\n=== SCANNING FOR VALID TASK_STRUCTS ===');
    let found = 0;
    const pageSize = 4096;
    const maxScan = 100 * 1024 * 1024; // Scan first 100MB

    for (let offset = 0; offset < maxScan && found < 5; offset += pageSize) {
        for (const slabOffset of KernelConstants.SLAB_OFFSETS) {
            const taskOffset = offset + slabOffset;
            if (taskOffset + KernelConstants.TASK_STRUCT_SIZE > memory.getTotalSize()) continue;

            // Read PID
            const pid = memory.readU32(taskOffset + KernelConstants.PID_OFFSET);
            if (pid > 0 && pid < 100000) {
                // Read comm to verify
                let comm = '';
                for (let i = 0; i < 16; i++) {
                    const byte = memory.readByte(taskOffset + KernelConstants.COMM_OFFSET + i);
                    if (byte === 0) break;
                    if (byte >= 32 && byte <= 126) {
                        comm += String.fromCharCode(byte);
                    } else {
                        comm = ''; // Invalid
                        break;
                    }
                }

                if (comm.length > 0) {
                    console.log(`Found PID ${pid}: "${comm}" at 0x${(taskOffset + KernelConstants.GUEST_RAM_START).toString(16)}`);

                    // Check mm_struct
                    const mmPtr = memory.readU64(taskOffset + KernelConstants.MM_OFFSET);
                    if (mmPtr) {
                        console.log(`  mm_struct: 0x${mmPtr.toString(16)}`);
                    }
                    found++;
                }
            }
        }
    }

    // Check known swapper_pg_dir
    console.log('\n=== CHECKING SWAPPER_PG_DIR ===');
    console.log('Known location: 0x136dbf000');
    dumpPageTableEntries(memory, 0x136dbf000, 'SWAPPER_PGD', 10);

    console.log('\n==================================================');
    console.log('           DIAGNOSTIC COMPLETE                    ');
    console.log('==================================================');

    // Close file
    fs.closeSync(fd);
}

main().catch(console.error);