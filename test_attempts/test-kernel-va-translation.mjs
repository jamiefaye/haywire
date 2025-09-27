#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Testing kernel VA translation using swapper_pg_dir\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Function to translate a kernel VA to PA using page table walk
function translateKernelVA(va, pgdPA) {
    const virtualAddr = BigInt(va);

    // Check if it's a kernel address
    if ((virtualAddr >> 48n) !== 0xffffn) {
        console.log(`  Not a kernel VA: 0x${virtualAddr.toString(16)}`);
        return null;
    }

    // Calculate PGD index (bits 47-39)
    const pgdIndex = Number((virtualAddr >> 39n) & 0x1FFn);

    // Read PGD entry
    const pgdOffset = (pgdPA - GUEST_RAM_START) + (pgdIndex * 8);
    const pgdBuf = Buffer.allocUnsafe(8);
    fs.readSync(fd, pgdBuf, 0, 8, pgdOffset);
    const pgdEntry = pgdBuf.readBigUint64LE(0);

    if ((pgdEntry & 3n) === 0n) {
        console.log(`  PGD[${pgdIndex}] is invalid`);
        return null;
    }

    console.log(`  PGD[${pgdIndex}]: 0x${pgdEntry.toString(16)}`);

    // If it's a block at PGD level (rare)
    if ((pgdEntry & 3n) === 1n) {
        const pa = Number(pgdEntry & PA_MASK) + Number(virtualAddr & 0x7FFFFFFFFFn);
        return pa;
    }

    // Get PUD table
    const pudTablePA = Number(pgdEntry & PA_MASK);
    const pudIndex = Number((virtualAddr >> 30n) & 0x1FFn);
    const pudOffset = (pudTablePA - GUEST_RAM_START) + (pudIndex * 8);

    const pudBuf = Buffer.allocUnsafe(8);
    fs.readSync(fd, pudBuf, 0, 8, pudOffset);
    const pudEntry = pudBuf.readBigUint64LE(0);

    if ((pudEntry & 3n) === 0n) {
        console.log(`  PUD[${pudIndex}] is invalid`);
        return null;
    }

    console.log(`  PUD[${pudIndex}]: 0x${pudEntry.toString(16)}`);

    // If it's a 1GB block
    if ((pudEntry & 3n) === 1n) {
        const pa = Number(pudEntry & PA_MASK) + Number(virtualAddr & 0x3FFFFFFFn);
        return pa;
    }

    // Get PMD table
    const pmdTablePA = Number(pudEntry & PA_MASK);
    const pmdIndex = Number((virtualAddr >> 21n) & 0x1FFn);
    const pmdOffset = (pmdTablePA - GUEST_RAM_START) + (pmdIndex * 8);

    const pmdBuf = Buffer.allocUnsafe(8);
    fs.readSync(fd, pmdBuf, 0, 8, pmdOffset);
    const pmdEntry = pmdBuf.readBigUint64LE(0);

    if ((pmdEntry & 3n) === 0n) {
        console.log(`  PMD[${pmdIndex}] is invalid`);
        return null;
    }

    console.log(`  PMD[${pmdIndex}]: 0x${pmdEntry.toString(16)}`);

    // If it's a 2MB block
    if ((pmdEntry & 3n) === 1n) {
        const pa = Number(pmdEntry & PA_MASK) + Number(virtualAddr & 0x1FFFFFn);
        return pa;
    }

    // Get PTE
    const pteTablePA = Number(pmdEntry & PA_MASK);
    const pteIndex = Number((virtualAddr >> 12n) & 0x1FFn);
    const pteOffset = (pteTablePA - GUEST_RAM_START) + (pteIndex * 8);

    const pteBuf = Buffer.allocUnsafe(8);
    fs.readSync(fd, pteBuf, 0, 8, pteOffset);
    const pteEntry = pteBuf.readBigUint64LE(0);

    if ((pteEntry & 3n) !== 3n) {
        console.log(`  PTE[${pteIndex}] is invalid`);
        return null;
    }

    console.log(`  PTE[${pteIndex}]: 0x${pteEntry.toString(16)}`);

    // Get final physical address
    const pageBase = Number(pteEntry & PA_MASK);
    const pageOffset = Number(virtualAddr & 0xFFFn);
    return pageBase + pageOffset;
}

// Test cases
const testVAs = [
    0xffff000080000000n,  // Should be in PGD[256] (vmalloc region)
    0xffff00007bc00000n,  // Another address we saw mapped
    0xffff7dffbf600000n,  // In PGD[507]
    0xffff7fffc0800000n,  // In PGD[511] (fixmap)
    0xffff800000000000n,  // Might not be mapped
];

console.log('Testing VA translations with swapper_pg_dir at 0x136DEB000:\n');

for (const va of testVAs) {
    console.log(`\nTesting VA: 0x${va.toString(16)}`);
    const pa = translateKernelVA(va, KNOWN_SWAPPER_PGD);

    if (pa) {
        console.log(`  ✓ Result: PA 0x${pa.toString(16)}`);

        // Verify it's in reasonable range
        if (pa >= GUEST_RAM_START && pa < 0x200000000) {
            console.log(`    In guest RAM range`);
        } else if (pa < GUEST_RAM_START) {
            console.log(`    Below RAM (MMIO region)`);
        } else {
            console.log(`    Above typical RAM`);
        }
    } else {
        console.log(`  ✗ Translation failed`);
    }
}

// Now test with a typical mm_struct address pattern
console.log('\n\nTesting typical mm_struct addresses:');
const typicalMMStructVAs = [
    0xffff0000c5570000n,  // Example pattern we might see
    0xffff000085570000n,  // Another pattern
];

for (const va of typicalMMStructVAs) {
    console.log(`\nTesting mm_struct VA: 0x${va.toString(16)}`);
    const pa = translateKernelVA(va, KNOWN_SWAPPER_PGD);

    if (pa) {
        console.log(`  ✓ Translates to PA: 0x${pa.toString(16)}`);
    } else {
        console.log(`  ✗ Not mapped in kernel PGD`);
        console.log(`    This mm_struct might be in kmalloc/vmalloc space that's not yet mapped`);
    }
}

fs.closeSync(fd);

console.log('\n=== CONCLUSION ===');
console.log('The kernel VA translation works for addresses that ARE mapped in swapper_pg_dir.');
console.log('But many mm_struct addresses might be in dynamically allocated regions');
console.log('that are not permanently mapped in the kernel PGD.');