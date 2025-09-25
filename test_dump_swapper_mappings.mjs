#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Complete Swapper PGD Mapping Dump ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

function readPage(fd, pa) {
    const buffer = Buffer.allocUnsafe(PAGE_SIZE);
    const offset = pa - GUEST_RAM_START;
    
    if (offset < 0 || offset + PAGE_SIZE > fs.fstatSync(fd).size) {
        return null;
    }
    
    try {
        fs.readSync(fd, buffer, 0, PAGE_SIZE, offset);
        return buffer;
    } catch (e) {
        return null;
    }
}

function formatSize(bytes) {
    if (bytes >= 0x40000000) return (bytes / 0x40000000).toFixed(1) + 'GB';
    if (bytes >= 0x100000) return (bytes / 0x100000).toFixed(1) + 'MB';
    if (bytes >= 0x1000) return (bytes / 0x1000).toFixed(0) + 'KB';
    return bytes + 'B';
}

function walkPTE(pteTablePA, pteVABase, indent = '      ') {
    const pteBuffer = readPage(fd, pteTablePA);
    if (!pteBuffer) {
        console.log(`${indent}[!] Failed to read PTE table at PA 0x${pteTablePA.toString(16)}`);
        return;
    }
    
    let mappedCount = 0;
    const ranges = [];
    let currentRange = null;
    
    for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
        const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
        if ((pteEntry & 0x3n) !== 0x3n) continue;
        
        const va = pteVABase + (BigInt(pteIdx) << 12n);
        const pa = Number(pteEntry & PA_MASK & ~0xFFFn);
        
        mappedCount++;
        
        // Group consecutive mappings
        if (currentRange && currentRange.endVA === va && currentRange.endPA === pa) {
            currentRange.endVA = va + 0x1000n;
            currentRange.endPA = pa + 0x1000;
            currentRange.count++;
        } else {
            if (currentRange) ranges.push(currentRange);
            currentRange = {
                startVA: va,
                endVA: va + 0x1000n,
                startPA: pa,
                endPA: pa + 0x1000,
                count: 1
            };
        }
    }
    if (currentRange) ranges.push(currentRange);
    
    if (mappedCount > 0) {
        console.log(`${indent}PTE Table at PA 0x${pteTablePA.toString(16)}: ${mappedCount} pages mapped`);
        for (const range of ranges.slice(0, 3)) {  // Show first 3 ranges
            const size = Number(range.endVA - range.startVA);
            console.log(`${indent}  VA 0x${range.startVA.toString(16)}-0x${range.endVA.toString(16)} (${formatSize(size)}) -> PA 0x${range.startPA.toString(16)}-0x${range.endPA.toString(16)}`);
        }
        if (ranges.length > 3) {
            console.log(`${indent}  ... and ${ranges.length - 3} more ranges`);
        }
    }
}

function walkPMD(pmdTablePA, pmdVABase, indent = '    ') {
    const pmdBuffer = readPage(fd, pmdTablePA);
    if (!pmdBuffer) {
        console.log(`${indent}[!] Failed to read PMD table at PA 0x${pmdTablePA.toString(16)}`);
        return;
    }
    
    let blockCount = 0;
    let tableCount = 0;
    
    for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
        const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
        if (pmdEntry === 0n) continue;
        
        const pmdVA = pmdVABase + (BigInt(pmdIdx) << 21n);
        
        if ((pmdEntry & 0x3n) === 0x1n) {
            // 2MB block
            blockCount++;
            if (blockCount <= 2) {
                const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                console.log(`${indent}PMD[${pmdIdx}]: VA 0x${pmdVA.toString(16)} -> 2MB block at PA 0x${blockPA.toString(16)}`);
            }
        } else if ((pmdEntry & 0x3n) === 0x3n) {
            // Page table
            tableCount++;
            const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
            if (tableCount <= 3) {
                console.log(`${indent}PMD[${pmdIdx}]: VA 0x${pmdVA.toString(16)} -> PTE table`);
                walkPTE(pteTablePA, pmdVA, indent + '  ');
            }
        }
    }
    
    if (blockCount > 2) {
        console.log(`${indent}... and ${blockCount - 2} more 2MB blocks`);
    }
    if (tableCount > 3) {
        console.log(`${indent}... and ${tableCount - 3} more PTE tables`);
    }
}

function walkPUD(pudTablePA, pudVABase, indent = '  ') {
    const pudBuffer = readPage(fd, pudTablePA);
    if (!pudBuffer) {
        console.log(`${indent}[!] Failed to read PUD table at PA 0x${pudTablePA.toString(16)}`);
        return;
    }
    
    console.log(`${indent}PUD Table at PA 0x${pudTablePA.toString(16)}:`);
    
    let blockCount = 0;
    let tableCount = 0;
    
    for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
        const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
        if (pudEntry === 0n) continue;
        
        const pudVA = pudVABase + (BigInt(pudIdx) << 30n);
        
        if ((pudEntry & 0x3n) === 0x1n) {
            // 1GB block
            blockCount++;
            const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
            console.log(`${indent}  PUD[${pudIdx}]: VA 0x${pudVA.toString(16)} -> 1GB block at PA 0x${blockPA.toString(16)}`);
        } else if ((pudEntry & 0x3n) === 0x3n) {
            // PMD table
            tableCount++;
            const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
            console.log(`${indent}  PUD[${pudIdx}]: VA 0x${pudVA.toString(16)} -> PMD table`);
            if (tableCount <= 2) {  // Only walk first 2 PMD tables to avoid huge output
                walkPMD(pmdTablePA, pudVA, indent + '    ');
            }
        }
    }
    
    if (tableCount > 2) {
        console.log(`${indent}  ... and ${tableCount - 2} more PMD tables (skipped for brevity)`);
    }
}

// Read and walk swapper_pg_dir
const pgdBuffer = readPage(fd, SWAPPER_PGD_PA);
if (!pgdBuffer) {
    console.log('Failed to read swapper_pg_dir');
    process.exit(1);
}

console.log(`Swapper PGD at PA 0x${SWAPPER_PGD_PA.toString(16)}`);
console.log('=' .repeat(70));

let nonZeroCount = 0;
const entries = [];

for (let pgdIdx = 0; pgdIdx < 512; pgdIdx++) {
    const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
    if (pgdEntry === 0n) continue;
    
    nonZeroCount++;
    entries.push({ idx: pgdIdx, entry: pgdEntry });
}

console.log(`\nNon-zero PGD entries: ${nonZeroCount}/512\n`);

for (const { idx, entry } of entries) {
    const pgdVA = BigInt(idx) << 39n;
    const vaStr = pgdVA.toString(16).padStart(16, '0');
    
    console.log(`\nPGD[${idx}]: VA 0x${vaStr} (${idx === 0 ? 'User space' : idx === 256 ? 'Kernel linear map' : idx === 507 ? 'Kernel fixmap' : idx === 511 ? 'Kernel modules' : 'Other'})`);
    console.log('-'.repeat(70));
    
    if ((entry & 0x3n) === 0x3n) {
        const pudTablePA = Number(entry & PA_MASK & ~0xFFFn);
        walkPUD(pudTablePA, pgdVA);
    } else if ((entry & 0x3n) === 0x1n) {
        // 512GB block (unlikely but possible)
        const blockPA = Number(entry & PA_MASK);
        console.log(`  512GB block at PA 0x${blockPA.toString(16)}`);
    } else {
        console.log(`  Unknown entry type: 0x${entry.toString(16)}`);
    }
}

fs.closeSync(fd);

console.log('\n' + '=' .repeat(70));
console.log('\nSummary:');
console.log('--------');
console.log('This shows all the virtual address mappings in the kernel.');
console.log('Key observations:');
console.log('- PGD[0]: User space mappings');
console.log('- PGD[256]: Kernel linear map (usually maps all physical RAM)');
console.log('- PGD[507]: Kernel fixmap region');
console.log('- PGD[511]: Kernel modules region');
console.log('');
console.log('If kernel structures aren\'t mapped here, they use:');
console.log('- Dynamic mappings (kmap/vmap)');
console.log('- Fixed offset from PA (implicit mapping)');
console.log('- Per-CPU page tables');