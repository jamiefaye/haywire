#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const GUEST_RAM_END = 0x40000000 + 0x180000000; // 6GB
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700];

console.log('=== VA->PA Mapping Verification for Straddling ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Build VA->PA mapping
class PageTableMapper {
    constructor(fd) {
        this.fd = fd;
        this.vaToPA = new Map();
    }
    
    buildMapping() {
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(this.fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);
        
        const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
        if (pgd256 === 0n) return;
        
        const pudTablePA = Number(pgd256 & PA_MASK & ~0xFFFn);
        this.walkPUD(pudTablePA, 0xFFFF800000000000n);
    }
    
    walkPUD(pudTablePA, pudVABase) {
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        } catch (e) {
            return;
        }
        
        // Only process PUD[1] and PUD[2] which contain our mappings
        for (let pudIdx = 1; pudIdx <= 2; pudIdx++) {
            const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
            if (pudEntry === 0n) continue;
            
            const pudVA = pudVABase + (BigInt(pudIdx) << 30n);
            
            if ((pudEntry & 0x3n) === 0x1n) {
                // 1GB block
                const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
                console.log(`PUD[${pudIdx}]: 1GB block VA 0x${pudVA.toString(16)} -> PA 0x${blockPA.toString(16)}`);
                
                // Map first few pages for testing
                for (let offset = 0; offset < 0x100000; offset += PAGE_SIZE) {
                    this.vaToPA.set(pudVA + BigInt(offset), blockPA + offset);
                }
            } else if ((pudEntry & 0x3n) === 0x3n) {
                // Table
                console.log(`PUD[${pudIdx}]: Table at VA 0x${pudVA.toString(16)}`);
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
        
        // Sample first few PMDs
        let mappedCount = 0;
        for (let pmdIdx = 0; pmdIdx < 512 && mappedCount < 10; pmdIdx++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
            if (pmdEntry === 0n) continue;
            
            const pmdVA = pmdVABase + (BigInt(pmdIdx) << 21n);
            
            if ((pmdEntry & 0x3n) === 0x1n) {
                // 2MB block
                const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
                if (mappedCount < 3) {
                    console.log(`  PMD[${pmdIdx}]: 2MB block VA 0x${pmdVA.toString(16)} -> PA 0x${blockPA.toString(16)}`);
                }
                
                // Map pages
                for (let offset = 0; offset < 0x200000; offset += PAGE_SIZE) {
                    this.vaToPA.set(pmdVA + BigInt(offset), blockPA + offset);
                }
                mappedCount++;
            } else if ((pmdEntry & 0x3n) === 0x3n) {
                // Table
                const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
                if (mappedCount < 3) {
                    console.log(`  PMD[${pmdIdx}]: PTE table at VA 0x${pmdVA.toString(16)}`);
                }
                this.walkPTE(pteTablePA, pmdVA);
                mappedCount++;
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
            
            const pageVA = pteVABase + (BigInt(pteIdx) << 12n);
            const pagePA = Number(pteEntry & PA_MASK & ~0xFFFn);
            this.vaToPA.set(pageVA, pagePA);
        }
    }
    
    lookupVA(va) {
        const pageVA = va & ~0xFFFn;
        const pageOffset = Number(va & 0xFFFn);
        const pagePA = this.vaToPA.get(pageVA);
        if (pagePA === undefined) return null;
        return pagePA + pageOffset;
    }
}

console.log('Building VA->PA mapping from page tables...\n');
const mapper = new PageTableMapper(fd);
mapper.buildMapping();

console.log(`\nTotal mapped pages: ${mapper.vaToPA.size}\n`);
console.log('='.repeat(70) + '\n');

// Test straddling scenarios
console.log('Testing Straddling Scenarios:\n');
console.log('For task_struct at offset 0x4700 (spans 3 pages):');
console.log('  Bytes 0x0000-0x08FF in page 1 (2304 bytes)');
console.log('  Bytes 0x0900-0x18FF in page 2 (4096 bytes)');
console.log('  Bytes 0x1900-0x2380 in page 3 (2688 bytes)');
console.log('');

// Test some actual VAs we might encounter
const testVAs = [
    0xFFFF800040000000n, // Start of linear map
    0xFFFF800040004700n, // Offset 0x4700 in first page of linear map
    0xFFFF800080000000n, // Start of second PUD region
    0xFFFF800080004700n, // Offset 0x4700 in second region
];

console.log('Testing VA mappings for straddling:\n');

for (const baseVA of testVAs) {
    console.log(`VA 0x${baseVA.toString(16)}:`);
    
    // Check if this VA and next pages are mapped
    const pa1 = mapper.lookupVA(baseVA);
    const pa2 = mapper.lookupVA(baseVA + 0x1000n);
    const pa3 = mapper.lookupVA(baseVA + 0x2000n);
    
    if (pa1) {
        console.log(`  Page 0: VA 0x${baseVA.toString(16)} -> PA 0x${pa1.toString(16)}`);
        
        // Check if PA is in valid RAM range
        if (pa1 >= GUEST_RAM_START && pa1 < GUEST_RAM_END) {
            console.log(`    ✓ PA is in guest RAM range`);
        } else {
            console.log(`    ✗ PA is OUTSIDE guest RAM (0x${GUEST_RAM_START.toString(16)}-0x${GUEST_RAM_END.toString(16)})`);
        }
    } else {
        console.log(`  Page 0: Not mapped`);
    }
    
    if (pa2) {
        console.log(`  Page 1: VA 0x${(baseVA + 0x1000n).toString(16)} -> PA 0x${pa2.toString(16)}`);
        
        // Check if contiguous or not
        if (pa1 && pa2 === pa1 + 0x1000) {
            console.log(`    ✓ Contiguous with previous page`);
        } else if (pa1) {
            console.log(`    ✗ NON-CONTIGUOUS (gap of 0x${Math.abs(pa2 - pa1 - 0x1000).toString(16)} bytes)`);
        }
        
        if (pa2 >= GUEST_RAM_START && pa2 < GUEST_RAM_END) {
            console.log(`    ✓ PA is in guest RAM range`);
        } else {
            console.log(`    ✗ PA is OUTSIDE guest RAM`);
        }
    } else {
        console.log(`  Page 1: Not mapped`);
    }
    
    if (pa3) {
        console.log(`  Page 2: VA 0x${(baseVA + 0x2000n).toString(16)} -> PA 0x${pa3.toString(16)}`);
        
        if (pa2 && pa3 === pa2 + 0x1000) {
            console.log(`    ✓ Contiguous with previous page`);
        } else if (pa2) {
            console.log(`    ✗ NON-CONTIGUOUS (gap of 0x${Math.abs(pa3 - pa2 - 0x1000).toString(16)} bytes)`);
        }
        
        if (pa3 >= GUEST_RAM_START && pa3 < GUEST_RAM_END) {
            console.log(`    ✓ PA is in guest RAM range`);
        } else {
            console.log(`    ✗ PA is OUTSIDE guest RAM`);
        }
    } else {
        console.log(`  Page 2: Not mapped`);
    }
    
    console.log('');
}

console.log('='.repeat(70) + '\n');
console.log('Key Questions Answered:\n');
console.log('1. Do our VAs map to reasonable PAs?');
console.log('   → Check above if PAs are within guest RAM range\n');
console.log('2. Does VA+4096 map to another reasonable PA?');
console.log('   → Check above if next pages are mapped and valid\n');
console.log('3. Are straddling pages contiguous?');
console.log('   → Check above if pages are marked contiguous or not\n');

// Now let's check some actual SLAB locations
console.log('='.repeat(70) + '\n');
console.log('Checking Known SLAB Locations:\n');

// Find some actual SLABs
const SLAB_SIZE = 0x8000;
let foundSLABs = [];

for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + 0x10000000 && foundSLABs.length < 5; pa += SLAB_SIZE) {
    const offset = pa - GUEST_RAM_START;
    if (offset < 0 || offset + SLAB_SIZE > fs.fstatSync(fd).size) continue;
    
    // Quick check for SLAB signature
    const buffer = Buffer.allocUnsafe(0x1000);
    try {
        fs.readSync(fd, buffer, 0, 0x1000, offset);
        
        // Check PID at offset 0x750
        const pid = buffer.readUint32LE(0x750);
        if (pid > 0 && pid < 32768) {
            foundSLABs.push(pa);
        }
    } catch (e) {}
}

console.log(`Found ${foundSLABs.length} potential SLAB pages\n`);

for (const slabPA of foundSLABs.slice(0, 3)) {
    console.log(`SLAB at PA 0x${slabPA.toString(16)}:`);
    
    // Calculate the VA for this PA (linear map)
    const slabVA = BigInt(slabPA - GUEST_RAM_START) + 0xFFFF800040000000n;
    console.log(`  Linear map VA: 0x${slabVA.toString(16)}`);
    
    // Check task at offset 0x4700 (the straddler)
    const taskVA = slabVA + 0x4700n;
    console.log(`  Task at offset 0x4700: VA 0x${taskVA.toString(16)}`);
    
    // Check the three pages this task would span
    const page1VA = taskVA & ~0xFFFn;
    const page2VA = page1VA + 0x1000n;
    const page3VA = page1VA + 0x2000n;
    
    const page1PA = mapper.lookupVA(page1VA);
    const page2PA = mapper.lookupVA(page2VA);
    const page3PA = mapper.lookupVA(page3VA);
    
    console.log(`    Page 1: VA 0x${page1VA.toString(16)} -> PA ${page1PA ? '0x' + page1PA.toString(16) : 'NOT MAPPED'}`);
    console.log(`    Page 2: VA 0x${page2VA.toString(16)} -> PA ${page2PA ? '0x' + page2PA.toString(16) : 'NOT MAPPED'}`);
    console.log(`    Page 3: VA 0x${page3VA.toString(16)} -> PA ${page3PA ? '0x' + page3PA.toString(16) : 'NOT MAPPED'}`);
    
    if (page1PA && page2PA && page3PA) {
        const contiguous = (page2PA === page1PA + 0x1000) && (page3PA === page2PA + 0x1000);
        if (contiguous) {
            console.log(`    → All 3 pages are CONTIGUOUS (easy case)`);
        } else {
            console.log(`    → Pages are NON-CONTIGUOUS (this is the 9% case!)`);
            console.log(`      Need VA mapping to follow the pages correctly`);
        }
    }
    console.log('');
}

fs.closeSync(fd);