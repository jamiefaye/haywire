#!/usr/bin/env node

import fs from 'fs';
import net from 'net';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Complete Kernel Address Space Exploration ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Get some known kernel symbols for validation
async function getKernelSymbols() {
    return new Promise((resolve) => {
        exec(`ssh vm "sudo cat /proc/kallsyms | grep -E '(init_task|init_pid_ns|swapper_pg_dir|vmalloc_start|vmalloc_end|page_offset_base)' | head -20"`, 
            (error, stdout) => {
                if (error) {
                    resolve([]);
                    return;
                }
                
                const symbols = [];
                const lines = stdout.trim().split('\n');
                for (const line of lines) {
                    const [addr, type, name] = line.split(' ');
                    symbols.push({
                        va: BigInt('0x' + addr),
                        name: name
                    });
                }
                resolve(symbols);
            }
        );
    });
}

class KernelMapper {
    constructor(fd) {
        this.fd = fd;
        this.vaToPA = new Map();
        this.regions = [];
    }
    
    exploreKernelSpace() {
        console.log('Exploring kernel virtual address space...\n');
        
        // Read swapper_pg_dir
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(this.fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);
        
        // ARM64 kernel space: PGD indices 256-511
        console.log('PGD Entries (Kernel Space):\n');
        console.log('Index | VA Start            | Type     | Details');
        console.log('------|---------------------|----------|--------------------------------');
        
        for (let pgdIdx = 0; pgdIdx < 512; pgdIdx++) {
            const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
            if (pgdEntry === 0n) continue;
            
            const pgdVA = BigInt(pgdIdx) << 39n;
            const vaStr = '0x' + pgdVA.toString(16).padStart(16, '0');
            
            let type = 'Unknown';
            let details = '';
            
            if ((pgdEntry & 0x3n) === 0x1n) {
                type = '512GB block';
                const blockPA = Number(pgdEntry & PA_MASK);
                details = `PA 0x${blockPA.toString(16)}`;
            } else if ((pgdEntry & 0x3n) === 0x3n) {
                type = 'Table';
                const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
                details = `PUD at PA 0x${pudTablePA.toString(16)}`;
                
                // Explore this PUD
                this.explorePUD(pudTablePA, pgdVA, pgdIdx);
            }
            
            console.log(`${pgdIdx.toString().padStart(5)} | ${vaStr} | ${type.padEnd(8)} | ${details}`);
        }
    }
    
    explorePUD(pudTablePA, pudVABase, pgdIdx) {
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        let mappedCount = 0;
        let blockCount = 0;
        let tableCount = 0;
        
        for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
            const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
            if (pudEntry === 0n) continue;
            
            mappedCount++;
            const pudVA = pudVABase + (BigInt(pudIdx) << 30n);
            
            if ((pudEntry & 0x3n) === 0x1n) {
                // 1GB block
                blockCount++;
                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                
                // Record this region
                this.regions.push({
                    vaStart: pudVA,
                    vaEnd: pudVA + 0x40000000n,
                    paStart: blockPA,
                    paEnd: blockPA + 0x40000000,
                    type: '1GB block',
                    pgdIdx,
                    pudIdx
                });
                
                // Map some pages for testing
                for (let offset = 0; offset < 0x100000; offset += PAGE_SIZE) {
                    this.vaToPA.set(pudVA + BigInt(offset), blockPA + offset);
                }
            } else if ((pudEntry & 0x3n) === 0x3n) {
                tableCount++;
                const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
                this.explorePMD(pmdTablePA, pudVA, pgdIdx, pudIdx);
            }
        }
        
        if (mappedCount > 0) {
            console.log(`      PGD[${pgdIdx}]: ${mappedCount} PUD entries (${blockCount} blocks, ${tableCount} tables)`);
        }
    }
    
    explorePMD(pmdTablePA, pmdVABase, pgdIdx, pudIdx) {
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        let mappedCount = 0;
        for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
            if (pmdEntry === 0n) continue;
            
            mappedCount++;
            const pmdVA = pmdVABase + (BigInt(pmdIdx) << 21n);
            
            if ((pmdEntry & 0x3n) === 0x1n) {
                // 2MB block
                const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                
                this.regions.push({
                    vaStart: pmdVA,
                    vaEnd: pmdVA + 0x200000n,
                    paStart: blockPA,
                    paEnd: blockPA + 0x200000,
                    type: '2MB block',
                    pgdIdx,
                    pudIdx,
                    pmdIdx
                });
                
                // Map pages
                for (let offset = 0; offset < 0x200000; offset += PAGE_SIZE) {
                    this.vaToPA.set(pmdVA + BigInt(offset), blockPA + offset);
                }
            } else if ((pmdEntry & 0x3n) === 0x3n) {
                // PTE table
                const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
                this.explorePTE(pteTablePA, pmdVA, pgdIdx, pudIdx, pmdIdx);
            }
        }
    }
    
    explorePTE(pteTablePA, pteVABase, pgdIdx, pudIdx, pmdIdx) {
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        let firstPA = null;
        let lastPA = null;
        let mappedCount = 0;
        
        for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
            const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
            if ((pteEntry & 0x3n) !== 0x3n) continue;
            
            mappedCount++;
            const pteVA = pteVABase + (BigInt(pteIdx) << 12n);
            const pagePA = Number(pteEntry & PA_MASK & ~0xFFFn);
            
            if (!firstPA) firstPA = pagePA;
            lastPA = pagePA;
            
            this.vaToPA.set(pteVA, pagePA);
        }
        
        if (mappedCount > 0) {
            this.regions.push({
                vaStart: pteVABase,
                vaEnd: pteVABase + (BigInt(mappedCount) << 12n),
                paStart: firstPA,
                paEnd: lastPA + PAGE_SIZE,
                type: `${mappedCount} pages`,
                pgdIdx,
                pudIdx,
                pmdIdx,
                sparse: true
            });
        }
    }
}

const mapper = new KernelMapper(fd);
mapper.exploreKernelSpace();

console.log('\n' + '='.repeat(70) + '\n');
console.log('Summary of Kernel Virtual Address Regions:\n');

// Group regions by type
const byType = {};
for (const region of mapper.regions) {
    if (!byType[region.type]) byType[region.type] = [];
    byType[region.type].push(region);
}

console.log('Region Types Found:');
for (const [type, regions] of Object.entries(byType)) {
    const totalSize = regions.reduce((sum, r) => sum + Number(r.vaEnd - r.vaStart), 0);
    console.log(`  ${type}: ${regions.length} regions, ${(totalSize / 1024 / 1024).toFixed(2)} MB total`);
}

// Analyze VA ranges
console.log('\nVirtual Address Ranges:');
const ranges = [
    { name: 'User space', start: 0x0n, end: 0x0000800000000000n },
    { name: 'Kernel linear map', start: 0xFFFF000000000000n, end: 0xFFFF800000000000n },
    { name: 'vmalloc/ioremap', start: 0xFFFF800000000000n, end: 0xFFFFC00000000000n },
    { name: 'Kernel modules', start: 0xFFFFC00000000000n, end: 0xFFFFFFFFFFFFFFFFn },
];

for (const range of ranges) {
    const mapped = mapper.regions.filter(r => r.vaStart >= range.start && r.vaStart < range.end);
    if (mapped.length > 0) {
        console.log(`  ${range.name}: ${mapped.length} mapped regions`);
        
        // Show first few
        for (const region of mapped.slice(0, 3)) {
            console.log(`    VA 0x${region.vaStart.toString(16)} (${region.type})`);
        }
        if (mapped.length > 3) {
            console.log(`    ... and ${mapped.length - 3} more`);
        }
    }
}

// Test known VAs
console.log('\n' + '='.repeat(70) + '\n');
console.log('Testing Known Kernel VAs:\n');

const knownVAs = [
    { va: 0xffff8000837624f0n, name: 'init_pid_ns (from kallsyms)' },
    { va: 0xffff800082fb8000n, name: 'swapper_pg_dir (estimated)' },
];

// Get more symbols from kallsyms
const symbols = await getKernelSymbols();
for (const sym of symbols) {
    knownVAs.push({ va: sym.va, name: sym.name });
}

console.log('Known VAs to validate against:');
for (const known of knownVAs) {
    const pa = mapper.vaToPA.get(known.va & ~0xFFFn);
    console.log(`  ${known.name}:`);
    console.log(`    VA: 0x${known.va.toString(16)}`);
    
    if (pa !== undefined) {
        console.log(`    PA: 0x${pa.toString(16)} ✓ MAPPED`);
        
        // Verify it's in valid RAM range
        if (pa >= GUEST_RAM_START && pa < GUEST_RAM_START + 0x180000000) {
            console.log(`    In guest RAM range ✓`);
        } else {
            console.log(`    OUTSIDE guest RAM! ✗`);
        }
    } else {
        console.log(`    NOT MAPPED ✗`);
        
        // Try to find nearby mappings
        let nearestVA = null;
        let nearestDist = 0x100000000n;
        
        for (const [va, _] of mapper.vaToPA) {
            const dist = known.va > va ? known.va - va : va - known.va;
            if (dist < nearestDist) {
                nearestDist = dist;
                nearestVA = va;
            }
        }
        
        if (nearestVA && nearestDist < 0x10000000n) {
            console.log(`    Nearest mapping: 0x${nearestVA.toString(16)} (${(Number(nearestDist) / 1024 / 1024).toFixed(2)} MB away)`);
        }
    }
}

// Check for linear mapping pattern
console.log('\n' + '='.repeat(70) + '\n');
console.log('Checking for Linear Mapping Pattern:\n');

let hasLinearMap = false;
for (const region of mapper.regions) {
    if (region.type.includes('GB block') || region.type.includes('MB block')) {
        // Check if VA - PA is constant (linear map)
        const offset = region.vaStart - BigInt(region.paStart);
        const expectedPA = Number(region.vaStart - offset);
        
        if (Math.abs(expectedPA - region.paStart) < 0x1000) {
            console.log(`Found potential linear map:`);
            console.log(`  VA range: 0x${region.vaStart.toString(16)} - 0x${region.vaEnd.toString(16)}`);
            console.log(`  PA range: 0x${region.paStart.toString(16)} - 0x${region.paEnd.toString(16)}`);
            console.log(`  Offset: 0x${offset.toString(16)}`);
            console.log(`  Formula: VA = PA + 0x${offset.toString(16)}`);
            hasLinearMap = true;
            break;
        }
    }
}

if (!hasLinearMap) {
    console.log('No clear linear mapping found!');
    console.log('The kernel may be using:');
    console.log('  - Dynamic/on-demand mappings');
    console.log('  - Per-CPU page tables');
    console.log('  - Temporary mappings (kmap)');
}

fs.closeSync(fd);