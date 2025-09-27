#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

console.log('Tracing kernel PGD entries to their final physical mappings\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Read the kernel PGD
const pgdFileOffset = KNOWN_SWAPPER_PGD - GUEST_RAM_START;
const pgdBuffer = Buffer.allocUnsafe(4096);
fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

// Helper to walk all the way down and collect final mappings
function walkToLeaves(tablePA, level, vaBase, indexPath = []) {
    const mappings = [];
    const tableOffset = tablePA - GUEST_RAM_START;
    const tableBuffer = Buffer.allocUnsafe(4096);

    try {
        fs.readSync(fd, tableBuffer, 0, 4096, tableOffset);
    } catch (e) {
        return mappings;
    }

    const levelNames = ['PGD', 'PUD', 'PMD', 'PTE'];
    const levelShifts = [39n, 30n, 21n, 12n];

    for (let i = 0; i < 512; i++) {
        const entry = tableBuffer.readBigUint64LE(i * 8);
        if (entry === 0n) continue;

        const type = entry & 0x3n;
        if (type === 0) continue; // Invalid

        const physAddr = Number(entry & PA_MASK);
        const currentVA = vaBase + (BigInt(i) << levelShifts[level]);

        if (type === 0x1n || level === 3) {
            // Block descriptor or PTE (leaf)
            const pageSizes = ['512GB', '1GB', '2MB', '4KB'];
            mappings.push({
                va: currentVA,
                pa: physAddr,
                size: pageSizes[level],
                path: [...indexPath, i],
                level: levelNames[level]
            });
        } else if (type === 0x3n && level < 3) {
            // Table descriptor, go deeper
            const childMappings = walkToLeaves(physAddr, level + 1, currentVA, [...indexPath, i]);
            mappings.push(...childMappings);
        }
    }

    return mappings;
}

// Process each kernel PGD entry
const kernelIndices = [256, 507, 511];

for (const pgdIdx of kernelIndices) {
    const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
    if (pgdEntry === 0n) continue;

    const pudTablePA = Number(pgdEntry & PA_MASK);
    const vaBase = 0xffff000000000000n + (BigInt(pgdIdx - 256) << 39n);

    console.log('='.repeat(70));
    console.log(`PGD[${pgdIdx}]: Walking to find all final mappings...`);
    console.log(`  VA range: 0x${vaBase.toString(16)} - 0x${(vaBase + (1n << 39n) - 1n).toString(16)}`);

    const mappings = walkToLeaves(pudTablePA, 1, vaBase, [pgdIdx]);

    // Analyze the mappings
    console.log(`\n  Found ${mappings.length} final mappings`);

    if (mappings.length > 0) {
        // Group by page size
        const bySize = {};
        mappings.forEach(m => {
            bySize[m.size] = (bySize[m.size] || 0) + 1;
        });
        console.log('  Page sizes:', bySize);

        // Find PA range
        const pas = mappings.map(m => m.pa).sort((a, b) => a - b);
        const minPA = pas[0];
        const maxPA = pas[pas.length - 1];

        console.log(`\n  Physical address range:`);
        console.log(`    Lowest PA:  0x${minPA.toString(16)}`);
        console.log(`    Highest PA: 0x${maxPA.toString(16)}`);

        // Identify what memory regions these PAs are in
        console.log(`\n  Physical memory regions mapped:`);
        const regions = new Map();

        mappings.forEach(m => {
            let region = 'Unknown';
            if (m.pa >= 0x40000000 && m.pa < 0x140000000) {
                region = 'Guest RAM (0x40000000 - 0x140000000)';
            } else if (m.pa >= 0x0 && m.pa < 0x40000000) {
                region = 'Below RAM (0x0 - 0x40000000) - MMIO/ROM';
            } else if (m.pa >= 0x140000000 && m.pa < 0x180000000) {
                region = 'High memory (0x140000000 - 0x180000000)';
            } else if (m.pa >= 0x180000000) {
                region = 'Very high (>0x180000000) - Extended memory';
            }

            if (!regions.has(region)) {
                regions.set(region, { count: 0, minPA: m.pa, maxPA: m.pa, minVA: m.va, maxVA: m.va });
            }
            const r = regions.get(region);
            r.count++;
            r.minPA = Math.min(r.minPA, m.pa);
            r.maxPA = Math.max(r.maxPA, m.pa);
            r.minVA = r.minVA < m.va ? r.minVA : m.va;
            r.maxVA = r.maxVA > m.va ? r.maxVA : m.va;
        });

        for (const [region, info] of regions) {
            console.log(`    ${region}:`);
            console.log(`      ${info.count} mappings`);
            console.log(`      PA: 0x${info.minPA.toString(16)} - 0x${info.maxPA.toString(16)}`);
            console.log(`      VA: 0x${info.minVA.toString(16)} - 0x${info.maxVA.toString(16)}`);
        }

        // Show first few and last few mappings
        console.log(`\n  Sample mappings:`);
        const samples = [...mappings.slice(0, 3), ...mappings.slice(-3)];
        const shown = new Set();
        samples.forEach(m => {
            const key = `${m.va.toString(16)}-${m.pa}`;
            if (!shown.has(key)) {
                shown.add(key);
                console.log(`    VA 0x${m.va.toString(16)} -> PA 0x${m.pa.toString(16)} (${m.size})`);
            }
        });

        // Check for patterns
        console.log(`\n  Pattern analysis:`);

        // Check if mappings are contiguous in PA
        let contiguous = true;
        let lastPA = -1;
        const pageSizeBytes = { '4KB': 0x1000, '2MB': 0x200000, '1GB': 0x40000000 };

        for (const m of mappings.slice(0, Math.min(10, mappings.length))) {
            if (lastPA !== -1) {
                const expectedPA = lastPA + (pageSizeBytes[m.size] || 0x1000);
                if (Math.abs(m.pa - expectedPA) > 0x10000) { // Allow some gap
                    contiguous = false;
                    break;
                }
            }
            lastPA = m.pa;
        }

        console.log(`    Contiguous physical: ${contiguous ? 'Yes (linear)' : 'No (scattered)'}`);

        // Check VA to PA relationship
        if (mappings.length >= 2) {
            const offset1 = mappings[0].va - BigInt(mappings[0].pa);
            const offset2 = mappings[1].va - BigInt(mappings[1].pa);
            const constantOffset = offset1 === offset2;
            if (constantOffset) {
                console.log(`    VA->PA offset: Constant (0x${offset1.toString(16)})`);
            } else {
                console.log(`    VA->PA offset: Variable (not linear)`);
            }
        }
    }
}

fs.closeSync(fd);

console.log('\n' + '='.repeat(70));
console.log('\n=== SUMMARY OF FINAL PHYSICAL DESTINATIONS ===');
console.log('PGD[256] (vmalloc/modules): Maps to scattered guest RAM pages');
console.log('PGD[507] (high kernel): Maps to both RAM and possibly MMIO regions');
console.log('PGD[511] (fixmap): Maps to MMIO/device memory outside normal RAM');