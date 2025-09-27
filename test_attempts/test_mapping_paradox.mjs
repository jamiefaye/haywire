#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== The Mapping Paradox ===\n');

const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');

// Get kernel symbols we KNOW are valid
async function getKnownSymbols() {
    return new Promise((resolve) => {
        exec(`ssh vm "sudo grep -E '(init_task|init_pid_ns|swapper_pg_dir|vector_table|__per_cpu_offset)' /proc/kallsyms | head -10"`,
            (error, stdout) => {
                if (error) {
                    resolve([]);
                    return;
                }
                const symbols = [];
                for (const line of stdout.trim().split('\n')) {
                    const [addr, type, name] = line.split(' ');
                    symbols.push({ va: BigInt('0x' + addr), name });
                }
                resolve(symbols);
            }
        );
    });
}

// Build complete mapping from all page tables
class CompleteMapper {
    constructor(fd) {
        this.fd = fd;
        this.mappedRanges = [];
        this.vaPages = new Set();
    }
    
    walkAllPageTables() {
        // Walk swapper_pg_dir
        this.walkPGD(SWAPPER_PGD_PA, 'swapper_pg_dir');
        
        // Also check other page directories
        this.walkPGD(0x136de8000, 'idmap_pg_dir');
        
        console.log(`\nTotal mapped VA pages found: ${this.vaPages.size}`);
        
        // Find VA ranges
        if (this.vaPages.size > 0) {
            const sortedVAs = Array.from(this.vaPages).sort((a, b) => {
                if (a < b) return -1;
                if (a > b) return 1;
                return 0;
            });
            
            // Group into ranges
            let rangeStart = sortedVAs[0];
            let rangeEnd = sortedVAs[0];
            
            for (let i = 1; i < sortedVAs.length; i++) {
                if (sortedVAs[i] === rangeEnd + 0x1000n) {
                    rangeEnd = sortedVAs[i];
                } else {
                    this.mappedRanges.push({ start: rangeStart, end: rangeEnd + 0x1000n });
                    rangeStart = sortedVAs[i];
                    rangeEnd = sortedVAs[i];
                }
            }
            this.mappedRanges.push({ start: rangeStart, end: rangeEnd + 0x1000n });
        }
    }
    
    walkPGD(pgdPA, name) {
        console.log(`\nWalking ${name}...`);
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        
        try {
            fs.readSync(this.fd, pgdBuffer, 0, PAGE_SIZE, pgdPA - GUEST_RAM_START);
        } catch (e) {
            console.log(`  Failed to read`);
            return;
        }
        
        let mappedCount = 0;
        for (let pgdIdx = 0; pgdIdx < 512; pgdIdx++) {
            const pgdEntry = pgdBuffer.readBigUint64LE(pgdIdx * 8);
            if (pgdEntry === 0n) continue;
            
            const pgdVA = BigInt(pgdIdx) << 39n;
            
            if ((pgdEntry & 0x3n) === 0x3n) {
                const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
                const count = this.walkPUD(pudTablePA, pgdVA);
                mappedCount += count;
            }
        }
        
        console.log(`  Total pages mapped: ${mappedCount}`);
    }
    
    walkPUD(pudTablePA, pudVABase) {
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        } catch (e) {
            return 0;
        }
        
        let count = 0;
        for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
            const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
            if (pudEntry === 0n) continue;
            
            const pudVA = pudVABase + (BigInt(pudIdx) << 30n);
            
            if ((pudEntry & 0x3n) === 0x1n) {
                // 1GB block
                for (let offset = 0n; offset < 0x40000000n; offset += 0x1000n) {
                    this.vaPages.add(pudVA + offset);
                    count++;
                }
            } else if ((pudEntry & 0x3n) === 0x3n) {
                count += this.walkPMD(Number(pudEntry & PA_MASK & ~0xFFFn), pudVA);
            }
        }
        return count;
    }
    
    walkPMD(pmdTablePA, pmdVABase) {
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        } catch (e) {
            return 0;
        }
        
        let count = 0;
        for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
            if (pmdEntry === 0n) continue;
            
            const pmdVA = pmdVABase + (BigInt(pmdIdx) << 21n);
            
            if ((pmdEntry & 0x3n) === 0x1n) {
                // 2MB block
                for (let offset = 0n; offset < 0x200000n; offset += 0x1000n) {
                    this.vaPages.add(pmdVA + offset);
                    count++;
                }
            } else if ((pmdEntry & 0x3n) === 0x3n) {
                count += this.walkPTE(Number(pmdEntry & PA_MASK & ~0xFFFn), pmdVA);
            }
        }
        return count;
    }
    
    walkPTE(pteTablePA, pteVABase) {
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        try {
            fs.readSync(this.fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        } catch (e) {
            return 0;
        }
        
        let count = 0;
        for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
            const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
            if ((pteEntry & 0x3n) !== 0x3n) continue;
            
            const va = pteVABase + (BigInt(pteIdx) << 12n);
            this.vaPages.add(va);
            count++;
        }
        return count;
    }
    
    isVAMapped(va) {
        const page = va & ~0xFFFn;
        return this.vaPages.has(page);
    }
}

// Main analysis
const mapper = new CompleteMapper(fd);
mapper.walkAllPageTables();

const symbols = await getKnownSymbols();

console.log('\n' + '='.repeat(70) + '\n');
console.log('The Paradox:\n');
console.log('Known valid kernel symbols (from /proc/kallsyms):');

let mappedCount = 0;
let unmappedCount = 0;

for (const sym of symbols) {
    const isMapped = mapper.isVAMapped(sym.va);
    const status = isMapped ? '✓ MAPPED' : '✗ NOT MAPPED';
    console.log(`  ${sym.name.padEnd(20)} at VA 0x${sym.va.toString(16)} ${status}`);
    
    if (isMapped) mappedCount++;
    else unmappedCount++;
}

console.log(`\nResult: ${mappedCount}/${symbols.length} symbols are mapped`);
console.log(`        ${unmappedCount}/${symbols.length} symbols are NOT mapped`);

if (unmappedCount > 0) {
    console.log('\n' + '='.repeat(70) + '\n');
    console.log('⚠️  PARADOX CONFIRMED!');
    console.log('\nThese kernel symbols are ACTIVELY IN USE but NOT MAPPED in page tables!');
    console.log('\nThis means one of:');
    console.log('1. The kernel uses different page tables we can\'t see');
    console.log('2. These addresses use a fixed offset from physical (implicit mapping)');
    console.log('3. The mappings are created dynamically when needed');
    console.log('4. We\'re looking at the wrong page tables');
    
    console.log('\nLet\'s check if there\'s a fixed offset...');
    
    // Check if there's a consistent PA-VA offset
    const swapperVA = 0xffff8000829eb000n;
    const swapperPA = 0x136deb000n;
    const offset = swapperVA - swapperPA;
    
    console.log(`\nSwapper_pg_dir: VA 0x${swapperVA.toString(16)} -> PA 0x${swapperPA.toString(16)}`);
    console.log(`Offset: 0x${offset.toString(16)}`);
    
    console.log('\nTesting if other symbols follow this offset:');
    for (const sym of symbols.slice(0, 3)) {
        const predictedPA = sym.va - offset;
        console.log(`  ${sym.name}: VA 0x${sym.va.toString(16)}`);
        console.log(`    Predicted PA: 0x${predictedPA.toString(16)}`);
        
        // Check if this PA is in valid range
        if (predictedPA >= BigInt(GUEST_RAM_START) && predictedPA < BigInt(GUEST_RAM_START + 0x180000000)) {
            console.log(`    ✓ In guest RAM range`);
            
            // Try to read from this PA
            const testBuffer = Buffer.allocUnsafe(8);
            try {
                fs.readSync(fd, testBuffer, 0, 8, Number(predictedPA - BigInt(GUEST_RAM_START)));
                const value = testBuffer.readBigUint64LE(0);
                console.log(`    Value at PA: 0x${value.toString(16)}`);
            } catch (e) {
                console.log(`    Failed to read`);
            }
        } else {
            console.log(`    ✗ Outside RAM range`);
        }
    }
}

fs.closeSync(fd);

console.log('\n' + '='.repeat(70) + '\n');
console.log('Implications for Process Discovery:');
console.log('-----------------------------------');
console.log('If kernel structures aren\'t in the page tables, then:');
console.log('1. We can\'t use VA->PA translation to follow straddled task_structs');
console.log('2. The 91% discovery rate is the maximum achievable');
console.log('3. We need to use different approaches (SLUB metadata, IDR, etc.)');
console.log('4. Or accept that some task_structs are unreconstructable');