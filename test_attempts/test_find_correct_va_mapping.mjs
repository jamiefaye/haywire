#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Finding Correct VA Mappings for Physical Pages ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Build complete PA->VA reverse mapping
class ReverseMapper {
    constructor(fd) {
        this.fd = fd;
        this.paToVA = new Map(); // Maps PA -> Set of VAs
        this.vaToPA = new Map(); // Maps VA -> PA
    }
    
    buildMapping() {
        console.log('Building complete PA->VA reverse mapping...\n');
        
        // Read swapper_pg_dir
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(this.fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);
        
        // Check all PGD entries (kernel space is 256-511)
        for (let pgdIdx = 256; pgdIdx < 512; pgdIdx++) {
            const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
            if (pgdEntry === 0n) continue;
            
            const pgdVA = BigInt(pgdIdx) << 39n;
            
            if ((pgdEntry & 0x3n) === 0x3n) {
                const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
                this.walkPUD(pudTablePA, pgdVA);
            }
        }
        
        console.log(`Found ${this.vaToPA.size} VA->PA mappings`);
        console.log(`Found ${this.paToVA.size} unique physical pages mapped\n`);
    }
    
    walkPUD(pudTablePA, pudVABase) {
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
            const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
            if (pudEntry === 0n) continue;
            
            const pudVA = pudVABase + (BigInt(pudIdx) << 30n);
            
            if ((pudEntry & 0x3n) === 0x1n) {
                // 1GB block
                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                for (let offset = 0; offset < 0x40000000; offset += PAGE_SIZE) {
                    const va = pudVA + BigInt(offset);
                    const pa = blockPA + offset;
                    this.addMapping(va, pa);
                }
            } else if ((pudEntry & 0x3n) === 0x3n) {
                const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
                this.walkPMD(pmdTablePA, pudVA);
            }
        }
    }
    
    walkPMD(pmdTablePA, pmdVABase) {
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
            if (pmdEntry === 0n) continue;
            
            const pmdVA = pmdVABase + (BigInt(pmdIdx) << 21n);
            
            if ((pmdEntry & 0x3n) === 0x1n) {
                // 2MB block
                const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                for (let offset = 0; offset < 0x200000; offset += PAGE_SIZE) {
                    const va = pmdVA + BigInt(offset);
                    const pa = blockPA + offset;
                    this.addMapping(va, pa);
                }
            } else if ((pmdEntry & 0x3n) === 0x3n) {
                const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
                this.walkPTE(pteTablePA, pmdVA);
            }
        }
    }
    
    walkPTE(pteTablePA, pteVABase) {
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
            const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
            if ((pteEntry & 0x3n) !== 0x3n) continue;
            
            const va = pteVABase + (BigInt(pteIdx) << 12n);
            const pa = Number(pteEntry & PA_MASK & ~0xFFFn);
            this.addMapping(va, pa);
        }
    }
    
    addMapping(va, pa) {
        this.vaToPA.set(va, pa);
        
        if (!this.paToVA.has(pa)) {
            this.paToVA.set(pa, new Set());
        }
        this.paToVA.get(pa).add(va);
    }
    
    findVAsForPA(pa) {
        // Find page-aligned PA
        const pagePA = pa & ~0xFFF;
        return this.paToVA.get(pagePA) || new Set();
    }
}

const mapper = new ReverseMapper(fd);
mapper.buildMapping();

console.log('='.repeat(70) + '\n');

// Test with known SLAB PAs
console.log('Testing Known SLAB Physical Addresses:\n');

const testPAs = [
    0x400e8000,  // First SLAB we found
    0x40128000,  // Second SLAB
    0x40160000,  // Third SLAB
];

for (const slabPA of testPAs) {
    console.log(`SLAB at PA 0x${slabPA.toString(16)}:`);
    
    // Find all VAs that map to this PA
    const vas = mapper.findVAsForPA(slabPA);
    if (vas.size > 0) {
        console.log(`  Found ${vas.size} VA mapping(s):`);
        for (const va of vas) {
            console.log(`    VA 0x${va.toString(16)}`);
        }
        
        // For each VA, check if the next pages are also mapped
        for (const va of vas) {
            console.log(`\n  Checking straddling for VA 0x${va.toString(16)}:`);
            
            // Task at offset 0x4700 would need 3 pages
            const taskVA = va + 0x4700n;
            const page1VA = taskVA & ~0xFFFn;
            const page2VA = page1VA + 0x1000n;
            const page3VA = page2VA + 0x1000n;
            
            const page1PA = mapper.vaToPA.get(page1VA);
            const page2PA = mapper.vaToPA.get(page2VA);
            const page3PA = mapper.vaToPA.get(page3VA);
            
            console.log(`    Task at 0x4700 needs:`);
            console.log(`      Page 1: VA 0x${page1VA.toString(16)} -> ${page1PA ? 'PA 0x' + page1PA.toString(16) : 'NOT MAPPED'}`);
            console.log(`      Page 2: VA 0x${page2VA.toString(16)} -> ${page2PA ? 'PA 0x' + page2PA.toString(16) : 'NOT MAPPED'}`);
            console.log(`      Page 3: VA 0x${page3VA.toString(16)} -> ${page3PA ? 'PA 0x' + page3PA.toString(16) : 'NOT MAPPED'}`);
            
            if (page1PA && page2PA && page3PA) {
                const contiguous = (page2PA === page1PA + 0x1000) && (page3PA === page2PA + 0x1000);
                console.log(`      Status: ${contiguous ? '✓ CONTIGUOUS' : '✗ NON-CONTIGUOUS'}`);
                
                if (!contiguous) {
                    console.log(`      This is why we miss this task_struct!`);
                    console.log(`      Physical scanner assumes contiguous but they're not.`);
                }
            } else {
                console.log(`      Status: ✗ INCOMPLETE MAPPING`);
            }
        }
    } else {
        console.log(`  ✗ NO VA MAPPINGS FOUND`);
        console.log(`  This PA is not mapped in kernel page tables!`);
    }
    console.log('');
}

console.log('='.repeat(70) + '\n');
console.log('Analysis:\n');

let mappedSLABs = 0;
let unmappedSLABs = 0;

// Check a larger sample
for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + 0x10000000; pa += 0x8000) {
    const vas = mapper.findVAsForPA(pa);
    if (vas.size > 0) {
        mappedSLABs++;
    } else {
        unmappedSLABs++;
    }
}

console.log(`Checked ${mappedSLABs + unmappedSLABs} potential SLAB locations:`);
console.log(`  ${mappedSLABs} have VA mappings`);
console.log(`  ${unmappedSLABs} have NO VA mappings`);
console.log('');

if (unmappedSLABs > mappedSLABs) {
    console.log('Most physical pages are NOT mapped in kernel page tables!');
    console.log('This means:');
    console.log('1. The kernel uses different page tables per process');
    console.log('2. We\'re only seeing swapper_pg_dir (kernel\'s page tables)');
    console.log('3. User process memory is mapped in their own page tables');
    console.log('4. Task_structs might be in kernel heap with on-demand mapping');
    console.log('');
    console.log('Solution: We need to either:');
    console.log('- Accept the 91% discovery rate from physical scanning');
    console.log('- Parse SLUB metadata to find page relationships');
    console.log('- Follow kernel data structures (IDR) that have pointers');
} else {
    console.log('Most physical pages ARE mapped!');
    console.log('We can use VA lookups to handle straddling.');
}

fs.closeSync(fd);