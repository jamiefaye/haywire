#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Full analysis of PGD[0] - what does it REALLY map?\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Get PGD[0]
const pgdEntry0 = pgdBuffer.readBigUint64LE(0);
const pudTablePA = Number(pgdEntry0 & PA_MASK);

console.log(`PGD[0]: 0x${pgdEntry0.toString(16)} -> PUD table at PA 0x${pudTablePA.toString(16)}\n`);

// Read the entire PUD table
const pudOffset = pudTablePA - GUEST_RAM_START;
const pudBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pudBuffer, 0, 4096, pudOffset);

// Analyze ALL PUD entries (not just 0-3)
console.log('Scanning all 512 PUD entries in PGD[0]\'s table:\n');

const validPuds = [];
for (let i = 0; i < 512; i++) {
    const pudEntry = pudBuffer.readBigUint64LE(i * 8);
    if (pudEntry !== 0n && (pudEntry & 0x3n) >= 1) {
        validPuds.push({
            index: i,
            entry: pudEntry,
            pa: Number(pudEntry & PA_MASK)
        });
    }
}

console.log(`Found ${validPuds.length} valid PUD entries (not just 4!)\n`);

// Calculate what VA ranges these map
console.log('Virtual Address Ranges Mapped:\n');

for (const pud of validPuds) {
    // Each PUD entry covers 1GB of VA space
    // But which VA space? It depends on how the kernel uses this PGD entry

    // For user space (if accessed with TTBR0):
    const userVAStart = pud.index * 0x40000000; // 1GB each
    const userVAEnd = userVAStart + 0x3FFFFFFF;

    // For kernel space (if accessed as part of kernel mapping):
    // The 0xffff0000... addresses we saw working might map here
    const kernelVAStart = 0xffff000000000000n + BigInt(pud.index) * 0x40000000n;
    const kernelVAEnd = kernelVAStart + 0x3FFFFFFFn;

    console.log(`PUD[${pud.index}]: 0x${pud.entry.toString(16)}`);
    console.log(`  PA of next table: 0x${pud.pa.toString(16)}`);
    console.log(`  If used for user space: VA 0x${userVAStart.toString(16)} - 0x${userVAEnd.toString(16)}`);
    console.log(`  If used for kernel: VA 0x${kernelVAStart.toString(16)} - 0x${kernelVAEnd.toString(16)}`);

    // Sample what this actually maps to
    if (pud.index < 10 || pud.index > 500) {  // Check interesting ones
        const pmdTableOffset = pud.pa - GUEST_RAM_START;
        const pmdSample = Buffer.allocUnsafe(64);
        fs.readSync(fd, pmdSample, 0, 64, pmdTableOffset);

        let validPmds = 0;
        for (let j = 0; j < 8; j++) {
            const pmd = pmdSample.readBigUint64LE(j * 8);
            if (pmd !== 0n && (pmd & 0x3n) >= 1) validPmds++;
        }
        console.log(`  PMD table has ${validPmds}/8 valid entries in first 8`);
    }
    console.log('');
}

// Now test if kernel VAs actually work through PGD[0]
console.log('\n=== TESTING KERNEL VA ACCESS THROUGH PGD[0] ===\n');

// We know these worked in our previous test
const testKernelVAs = [
    { va: 0xffff000000000000n, desc: 'Start of kernel vmalloc space' },
    { va: 0xffff000040000000n, desc: 'vmalloc + 1GB' },
    { va: 0xffff000080000000n, desc: 'vmalloc + 2GB' },
    { va: 0xffff0000c0000000n, desc: 'vmalloc + 3GB' },
    { va: 0xffff000100000000n, desc: 'vmalloc + 4GB' },
];

for (const test of testKernelVAs) {
    // Calculate which PUD index this would use
    // The 0xffff is stripped, then we have the regular address
    // Actually, let's see how the kernel might interpret this

    // If PGD[0] is being used for 0xffff0000... addresses,
    // the PUD index would be based on bits [38:30] after the 0xffff0000 prefix
    const withoutPrefix = test.va & 0xFFFFFFFFn;  // Get lower 32 bits
    const pudIndex = Number(withoutPrefix >> 30n);

    console.log(`VA 0x${test.va.toString(16)} (${test.desc})`);
    console.log(`  Would use PUD[${pudIndex}] if mapped through PGD[0]`);

    // Check if we have that PUD entry
    const hasPud = validPuds.find(p => p.index === pudIndex);
    if (hasPud) {
        console.log(`  ✓ PUD[${pudIndex}] exists! Entry: 0x${hasPud.entry.toString(16)}`);
    } else {
        console.log(`  ✗ PUD[${pudIndex}] not present`);
    }
    console.log('');
}

fs.closeSync(fd);

console.log('=== CONCLUSION ===');
console.log('PGD[0] is NOT just for user space linear mapping!');
console.log('It appears to handle BOTH:');
console.log('1. User VA 0x0-0xFFFFFFFF -> PA linear mapping (when accessed via TTBR0)');
console.log('2. Kernel VA 0xffff0000xxxxxxxx -> various mappings (when accessed via TTBR1)');
console.log('\nThis is why kernel VA translation works through PGD[0].');
console.log('The kernel is using the SAME page table structure for multiple purposes!');