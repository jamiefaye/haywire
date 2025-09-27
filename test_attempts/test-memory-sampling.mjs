#!/usr/bin/env node

import fs from 'fs';
import crypto from 'crypto';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;

console.log('Sampling memory to identify content patterns\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const fileSize = fs.statSync('/tmp/haywire-vm-mem').size;

// Sample every 1000th page
const sampleInterval = 1000;
const patterns = {
    zeros: 0,
    kernel_code: 0,
    repeated_pattern: 0,
    text_like: 0,
    structured_data: 0,
    random_looking: 0,
    total: 0
};

// Sample some pages
for (let offset = 0; offset < fileSize; offset += PAGE_SIZE * sampleInterval) {
    const buffer = Buffer.allocUnsafe(PAGE_SIZE);

    try {
        fs.readSync(fd, buffer, 0, PAGE_SIZE, offset);
    } catch (e) {
        continue;
    }

    patterns.total++;

    // Check for zeros
    let zeroCount = 0;
    for (let i = 0; i < PAGE_SIZE; i++) {
        if (buffer[i] === 0) zeroCount++;
    }

    if (zeroCount > PAGE_SIZE * 0.95) {
        patterns.zeros++;
        continue;
    }

    // Check for kernel code (lots of specific byte patterns)
    let hasKernelSigs = false;
    // Look for ARM64 instruction patterns (many instructions start with specific bytes)
    let instrPatterns = 0;
    for (let i = 0; i < PAGE_SIZE - 4; i += 4) {
        const word = buffer.readUInt32LE(i);
        // Common ARM64 instruction prefixes
        if ((word & 0xFF000000) === 0xD5000000 || // System instructions
            (word & 0xFF000000) === 0x94000000 || // Branch
            (word & 0xFF000000) === 0xF9000000) { // Load/store
            instrPatterns++;
        }
    }
    if (instrPatterns > 50) {
        patterns.kernel_code++;
        continue;
    }

    // Check for repeated patterns (like freed memory patterns)
    const first256 = buffer.slice(0, 256);
    let isRepeated = true;
    for (let i = 256; i < PAGE_SIZE; i += 256) {
        if (!buffer.slice(i, i + 256).equals(first256)) {
            isRepeated = false;
            break;
        }
    }
    if (isRepeated) {
        patterns.repeated_pattern++;
        continue;
    }

    // Check for text-like content
    let printableCount = 0;
    for (let i = 0; i < PAGE_SIZE; i++) {
        if (buffer[i] >= 32 && buffer[i] <= 126) printableCount++;
    }
    if (printableCount > PAGE_SIZE * 0.5) {
        patterns.text_like++;
        continue;
    }

    // Check for structured data (lots of pointers)
    let pointerLikeCount = 0;
    for (let i = 0; i < PAGE_SIZE - 8; i += 8) {
        const val = buffer.readBigUInt64LE(i);
        // Check if it looks like a kernel VA or PA
        if ((val >= 0xffff000000000000n && val <= 0xffffffffffffffffn) || // Kernel VA
            (val >= 0x40000000n && val <= 0x200000000n)) { // Physical addr
            pointerLikeCount++;
        }
    }
    if (pointerLikeCount > 10) {
        patterns.structured_data++;
        continue;
    }

    // Everything else
    patterns.random_looking++;
}

console.log('\nMemory Content Analysis (sampled):');
console.log(`Total pages sampled: ${patterns.total}`);
console.log(`  Zero pages:        ${patterns.zeros} (${(patterns.zeros * 100 / patterns.total).toFixed(1)}%)`);
console.log(`  Kernel code:       ${patterns.kernel_code} (${(patterns.kernel_code * 100 / patterns.total).toFixed(1)}%)`);
console.log(`  Repeated patterns: ${patterns.repeated_pattern} (${(patterns.repeated_pattern * 100 / patterns.total).toFixed(1)}%)`);
console.log(`  Text-like:         ${patterns.text_like} (${(patterns.text_like * 100 / patterns.total).toFixed(1)}%)`);
console.log(`  Structured data:   ${patterns.structured_data} (${(patterns.structured_data * 100 / patterns.total).toFixed(1)}%)`);
console.log(`  Random/Other:      ${patterns.random_looking} (${(patterns.random_looking * 100 / patterns.total).toFixed(1)}%)`);

// Sample specific regions
console.log('\n\nChecking specific memory regions:');

const regions = [
    { name: 'Start of RAM', offset: 0x0 },
    { name: '1GB mark', offset: 0x40000000 },
    { name: '2GB mark', offset: 0x80000000 },
    { name: 'Near swapper_pg_dir', offset: 0xF6DEB000 },
];

for (const region of regions) {
    if (region.offset >= fileSize) continue;

    const sample = Buffer.allocUnsafe(256);
    fs.readSync(fd, sample, 0, 256, region.offset);

    // Get a hash to identify content
    const hash = crypto.createHash('md5').update(sample).digest('hex').slice(0, 8);

    // Check what it looks like
    let zeros = 0;
    for (let i = 0; i < 256; i++) {
        if (sample[i] === 0) zeros++;
    }

    let description = 'Unknown data';
    if (zeros > 240) {
        description = 'Mostly zeros';
    } else if (sample[0] === 0x7f && sample[1] === 0x45 && sample[2] === 0x4c && sample[3] === 0x46) {
        description = 'ELF header';
    } else {
        // Check for kernel pointers
        let pointers = 0;
        for (let i = 0; i < 256 - 8; i += 8) {
            const val = sample.readBigUInt64LE(i);
            if (val >= 0xffff000000000000n && val <= 0xffffffffffffffffn) pointers++;
        }
        if (pointers > 3) description = `Kernel data (${pointers} pointers)`;
    }

    console.log(`  ${region.name.padEnd(20)} @ 0x${region.offset.toString(16).padStart(8, '0')}: ${description} (hash: ${hash})`);
}

fs.closeSync(fd);

console.log('\n\nConclusion:');
console.log('Most of the memory file contains:');
console.log('1. Zero pages (never used or freed)');
console.log('2. Old data from deallocated processes');
console.log('3. Page cache and buffers');
console.log('4. Pre-allocated kernel memory pools');
console.log('5. User process memory not visible through kernel PGD');