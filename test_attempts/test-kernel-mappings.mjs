#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Examining the 3 kernel PGD entries\n');
console.log('ARM64 kernel VA space: 0xffff000000000000 - 0xffffffffffffffff');
console.log('Each PGD entry covers 512GB of VA space\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// The three kernel entries
const kernelIndices = [256, 507, 511];

for (const pgdIdx of kernelIndices) {
    const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
    const pudTablePA = Number(pgdEntry & PA_MASK);

    // Calculate the VA range this PGD entry covers
    // In kernel space (TTBR1), index 256 is the first kernel entry
    // Kernel VAs start at 0xffff000000000000
    const vaBase = 0xffff000000000000n + (BigInt(pgdIdx - 256) << 39n);
    const vaEnd = vaBase + (1n << 39n) - 1n;

    console.log('='.repeat(70));
    console.log(`PGD[${pgdIdx}]: 0x${pgdEntry.toString(16)}`);
    console.log(`  PUD table at PA: 0x${pudTablePA.toString(16)}`);
    console.log(`  VA range: 0x${vaBase.toString(16)} - 0x${vaEnd.toString(16)}`);

    // Identify the kernel region based on index
    let regionName = '';
    if (pgdIdx === 256) {
        regionName = 'MODULES/VMALLOC/VMEMMAP region';
        console.log(`  This is the ${regionName}`);
        console.log(`  - modules: loadable kernel modules`);
        console.log(`  - vmalloc: dynamically allocated kernel memory`);
        console.log(`  - vmemmap: struct page array for all physical memory`);
    } else if (pgdIdx === 507) {
        regionName = 'Near top of kernel space';
        console.log(`  This is ${regionName}`);
        console.log(`  - Could be KASAN shadow, kernel text, or other mappings`);
    } else if (pgdIdx === 511) {
        regionName = 'FIXMAP region';
        console.log(`  This is the ${regionName} (top of kernel VA space)`);
        console.log(`  - fixmap: fixed virtual addresses for special purposes`);
        console.log(`  - early_fixmap: used during boot`);
        console.log(`  - FDT, early console, etc.`);
    }

    // Read PUD table
    const pudOffset = pudTablePA - GUEST_RAM_START;
    const pudBuffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, pudBuffer, 0, 4096, pudOffset);

    // Find valid PUD entries
    const validPuds = [];
    for (let i = 0; i < 512; i++) {
        const pudEntry = pudBuffer.readBigUint64LE(i * 8);
        if (pudEntry !== 0n && (pudEntry & 0x3n) >= 1) {
            validPuds.push({idx: i, entry: pudEntry});
        }
    }

    console.log(`\n  PUD level: ${validPuds.length} valid entries`);

    // Examine each valid PUD
    for (const pud of validPuds.slice(0, 5)) { // Limit to first 5
        const pudPA = Number(pud.entry & PA_MASK);
        const pudType = pud.entry & 0x3n;
        const pudVABase = vaBase + (BigInt(pud.idx) << 30n); // Each PUD covers 1GB

        console.log(`\n  PUD[${pud.idx}]: 0x${pud.entry.toString(16)}`);
        console.log(`    VA range: 0x${pudVABase.toString(16)} - 0x${(pudVABase + (1n << 30n) - 1n).toString(16)}`);

        if (pudType === 0x3n) {
            // It's a table, go to PMD level
            console.log(`    Type: Table -> PMD at PA 0x${pudPA.toString(16)}`);

            // Read PMD table
            const pmdOffset = pudPA - GUEST_RAM_START;
            const pmdBuffer = Buffer.allocUnsafe(4096);
            fs.readSync(fd, pmdBuffer, 0, 4096, pmdOffset);

            // Count valid PMDs
            let validPmds = 0;
            let firstPmdIdx = -1;
            let firstPmdEntry = 0n;
            for (let i = 0; i < 512; i++) {
                const pmdEntry = pmdBuffer.readBigUint64LE(i * 8);
                if (pmdEntry !== 0n && (pmdEntry & 0x3n) >= 1) {
                    validPmds++;
                    if (firstPmdIdx === -1) {
                        firstPmdIdx = i;
                        firstPmdEntry = pmdEntry;
                    }
                }
            }

            console.log(`    PMD level: ${validPmds} valid entries`);

            if (firstPmdIdx !== -1) {
                const pmdType = firstPmdEntry & 0x3n;
                const pmdPA = Number(firstPmdEntry & PA_MASK);
                const pmdVA = pudVABase + (BigInt(firstPmdIdx) << 21n); // Each PMD is 2MB

                console.log(`    First PMD[${firstPmdIdx}]: VA 0x${pmdVA.toString(16)}`);

                if (pmdType === 0x1n) {
                    console.log(`      2MB block -> PA 0x${pmdPA.toString(16)}`);
                } else if (pmdType === 0x3n) {
                    console.log(`      PTE table -> PA 0x${pmdPA.toString(16)}`);

                    // Sample a PTE
                    const pteOffset = pmdPA - GUEST_RAM_START;
                    const pteBuffer = Buffer.allocUnsafe(64);
                    fs.readSync(fd, pteBuffer, 0, 64, pteOffset);

                    for (let i = 0; i < 8; i++) {
                        const pte = pteBuffer.readBigUint64LE(i * 8);
                        if (pte !== 0n && (pte & 0x3n) >= 1) {
                            const ptePA = Number(pte & PA_MASK);
                            const pteVA = pmdVA + (BigInt(i) << 12n);
                            console.log(`      First PTE[${i}]: VA 0x${pteVA.toString(16)} -> PA 0x${ptePA.toString(16)}`);

                            // Try to identify what this might be based on PA
                            if (ptePA >= 0x40000000 && ptePA < 0x140000000) {
                                console.log(`        (Maps to guest RAM)`);
                            } else {
                                console.log(`        (Maps outside normal RAM - could be MMIO)`);
                            }
                            break;
                        }
                    }
                }
            }
        } else if (pudType === 0x1n) {
            console.log(`    Type: 1GB huge page -> PA 0x${pudPA.toString(16)}`);
        }
    }
    console.log('');
}

fs.closeSync(fd);

console.log('='.repeat(70));
console.log('\n=== SUMMARY ===');
console.log('The 3 kernel PGD entries map:');
console.log('1. PGD[256]: Low kernel space (modules, vmalloc, vmemmap)');
console.log('2. PGD[507]: High kernel space (possibly KASAN, kernel text)');
console.log('3. PGD[511]: Top of kernel space (fixmap region)');
console.log('\nThese are sparse because the kernel only maps what it needs,');
console.log('unlike the complete 4GB linear mapping we saw in user space.');