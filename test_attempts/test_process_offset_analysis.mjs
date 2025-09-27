#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;

// SLAB offsets we check
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];
const PAGE_STRADDLE_OFFSETS = [0x0, 0x380, 0x700];

console.log('=== Process Offset Analysis ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');
const stats = fs.fstatSync(fd);
const fileSize = stats.size;

console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB\n`);

// Track where we find processes
const foundProcesses = [];
const offsetHistogram = new Map();

console.log('Scanning for processes (first 2GB)...\n');

// Scan first 2GB more thoroughly
const scanSize = Math.min(2 * 1024 * 1024 * 1024, fileSize);
let pagesScanned = 0;

for (let pageStart = 0; pageStart < scanSize; pageStart += PAGE_SIZE) {
    pagesScanned++;

    if (pageStart % (100 * 1024 * 1024) === 0) {
        process.stdout.write(`  Scanning ${(pageStart / (1024 * 1024)).toFixed(0)}MB...\r`);
    }

    // Check all our offsets
    const offsetsToCheck = [...SLAB_OFFSETS, ...PAGE_STRADDLE_OFFSETS];

    for (const offset of offsetsToCheck) {
        const taskOffset = pageStart + offset;

        if (taskOffset + TASK_STRUCT_SIZE > fileSize) continue;

        try {
            // Read PID
            const pidBuffer = Buffer.allocUnsafe(4);
            fs.readSync(fd, pidBuffer, 0, 4, taskOffset + PID_OFFSET);
            const pid = pidBuffer.readUint32LE(0);

            if (pid < 1 || pid > 32768) continue;

            // Read comm
            const commBuffer = Buffer.allocUnsafe(16);
            fs.readSync(fd, commBuffer, 0, 16, taskOffset + COMM_OFFSET);
            const comm = commBuffer.toString('ascii').split('\0')[0];

            // Validate comm - should be printable ASCII
            if (!comm || comm.length === 0 || comm.length > 15) continue;
            if (!/^[\x20-\x7E]+$/.test(comm)) continue;

            // Found a valid process!
            const pageOffset = offset;
            const pageAlignedOffset = taskOffset % PAGE_SIZE;

            foundProcesses.push({
                pid,
                comm,
                taskOffset,
                pageStart,
                offsetInPage: pageAlignedOffset,
                slabOffset: offset,
                pa: taskOffset + GUEST_RAM_START
            });

            // Track which offset pattern found it
            const key = `0x${offset.toString(16)}`;
            offsetHistogram.set(key, (offsetHistogram.get(key) || 0) + 1);

        } catch (e) {
            // Ignore read errors
        }
    }
}

console.log(`\n\nFound ${foundProcesses.length} processes\n`);

// Analyze offset distribution
console.log('=== Offset Distribution ===');
console.log('Offset    Count   Percentage   Type');
console.log('-------   -----   ----------   ----');

const total = foundProcesses.length;
for (const [offset, count] of offsetHistogram.entries()) {
    const pct = ((count / total) * 100).toFixed(1);
    let type = 'unknown';

    const val = parseInt(offset);
    if (val === 0x0) type = 'First in SLAB';
    else if (val === 0x2380) type = 'Second in SLAB (straddles)';
    else if (val === 0x4700) type = 'Third in SLAB';
    else if (val === 0x380) type = 'Page straddle 1';
    else if (val === 0x700) type = 'Page straddle 2';

    console.log(`${offset.padEnd(8)}  ${count.toString().padEnd(5)}   ${pct.padEnd(10)}%  ${type}`);
}

// Show which offsets would straddle pages
console.log('\n=== Page Straddling Analysis ===');
console.log('For task_struct size 0x2380 (9088 bytes):\n');

for (const offset of SLAB_OFFSETS) {
    const endOffset = offset + TASK_STRUCT_SIZE;
    const startPage = Math.floor(offset / PAGE_SIZE);
    const endPage = Math.floor((endOffset - 1) / PAGE_SIZE);

    console.log(`Offset 0x${offset.toString(16)} (${offset}):`);
    console.log(`  Starts in page ${startPage} (offset 0x${(offset % PAGE_SIZE).toString(16)})`);
    console.log(`  Ends in page ${endPage} (offset 0x${((endOffset - 1) % PAGE_SIZE).toString(16)})`);

    if (startPage !== endPage) {
        console.log(`  ** STRADDLES ${endPage - startPage + 1} PAGES **`);

        // Check critical fields
        const pidOffset = offset + PID_OFFSET;
        const commOffset = offset + COMM_OFFSET;
        const pidPage = Math.floor(pidOffset / PAGE_SIZE);
        const commPage = Math.floor(commOffset / PAGE_SIZE);

        console.log(`  PID field: page ${pidPage} offset 0x${(pidOffset % PAGE_SIZE).toString(16)}`);
        console.log(`  comm field: page ${commPage} offset 0x${(commOffset % PAGE_SIZE).toString(16)}`);

        if (pidPage !== commPage) {
            console.log(`  !! PID and comm in DIFFERENT PAGES !!`);
        }
    }
    console.log('');
}

// Show sample of found processes
console.log('=== Sample Processes Found ===');
console.log('First 10 processes:');
for (let i = 0; i < Math.min(10, foundProcesses.length); i++) {
    const p = foundProcesses[i];
    console.log(`  PID ${p.pid}: ${p.comm}`);
    console.log(`    PA: 0x${p.pa.toString(16)}, SLAB offset: 0x${p.slabOffset.toString(16)}`);
}

// Compare with ground truth
try {
    const groundTruth = fs.readFileSync('ground_truth_processes.txt', 'utf-8')
        .split('\n')
        .filter(line => line.trim())
        .length;

    console.log(`\n=== Comparison ===`);
    console.log(`Ground truth: ${groundTruth} processes`);
    console.log(`Found: ${foundProcesses.length} processes`);
    console.log(`Discovery rate: ${((foundProcesses.length / groundTruth) * 100).toFixed(1)}%`);
} catch (e) {
    console.log('\nNo ground truth file for comparison');
}

fs.closeSync(fd);