#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Checking if SLAB regions have PTE entries ===\n');

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

// From previous test, we found PMD[27] has entry 0x100000013819f003
const pmdEntry = 0x100000013819f003n;

if ((pmdEntry & 0x3n) === 0x3n) {
    console.log('PMD[27] points to a PTE table!');
    const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
    console.log(`PTE table at PA: 0x${pteTablePA.toString(16)}\n`);
    
    const pteBuffer = readPage(fd, pteTablePA);
    
    // Check all PTE entries
    let mappedCount = 0;
    const mappings = [];
    
    for (let i = 0; i < 512; i++) {
        const pte = pteBuffer.readBigUint64LE(i * 8);
        if ((pte & 0x3n) === 0x3n) {
            const pa = Number(pte & PA_MASK & ~0xFFFn);
            const va = 0xffff800083600000n + BigInt(i * 0x1000);
            mappedCount++;
            mappings.push({ idx: i, va, pa });
        }
    }
    
    console.log(`Found ${mappedCount} mapped pages in this PTE table\n`);
    
    // Show some mappings
    console.log('Sample mappings:');
    for (const m of mappings.slice(0, 10)) {
        console.log(`  PTE[${m.idx}]: VA 0x${m.va.toString(16)} -> PA 0x${m.pa.toString(16)}`);
    }
    
    // Check if init_task VA would be in this range
    const initTaskVA = 0xffff800083739840n;
    const pteStartVA = 0xffff800083600000n;
    const pteEndVA = 0xffff800083800000n;
    
    console.log('\n' + '='.repeat(70) + '\n');
    console.log('Critical finding:');
    
    if (initTaskVA >= pteStartVA && initTaskVA < pteEndVA) {
        const pteIdx = Number((initTaskVA - pteStartVA) >> 12n);
        console.log(`init_task VA 0x${initTaskVA.toString(16)} IS in this PTE range!`);
        console.log(`It would be at PTE[${pteIdx}]`);
        
        const initPte = pteBuffer.readBigUint64LE(pteIdx * 8);
        if ((initPte & 0x3n) === 0x3n) {
            const initPA = Number(initPte & PA_MASK & ~0xFFFn);
            console.log(`PTE[${pteIdx}]: 0x${initPte.toString(16)}`);
            console.log(`Maps to PA: 0x${initPA.toString(16)}`);
            console.log('');
            console.log('✅ SLAB regions DO have page table entries!');
            console.log('But wait... let\'s check if pages are contiguous...');
            
            // Check next few pages
            console.log('\nChecking if following pages are contiguous:');
            for (let i = 0; i < 5; i++) {
                const nextIdx = pteIdx + i;
                const nextPte = pteBuffer.readBigUint64LE(nextIdx * 8);
                if ((nextPte & 0x3n) === 0x3n) {
                    const nextPA = Number(nextPte & PA_MASK & ~0xFFFn);
                    const expectedPA = initPA + (i * 0x1000);
                    const isContiguous = nextPA === expectedPA;
                    console.log(`  PTE[${nextIdx}]: PA 0x${nextPA.toString(16)} ${isContiguous ? '✓ contiguous' : `✗ gap! (expected 0x${expectedPA.toString(16)})`}`);
                    
                    if (!isContiguous) {
                        console.log('\n🔥 FOUND IT! Non-contiguous pages in SLAB region!');
                        console.log('This is why some task_structs fail - the pages are not physically contiguous!');
                        break;
                    }
                }
            }
        } else {
            console.log(`PTE[${pteIdx}] is not valid: 0x${initPte.toString(16)}`);
        }
    } else {
        console.log(`init_task VA 0x${initTaskVA.toString(16)} is NOT in this PTE range`);
        console.log(`PTE range: 0x${pteStartVA.toString(16)} - 0x${pteEndVA.toString(16)}`);
    }
} else if ((pmdEntry & 0x3n) === 0x1n) {
    console.log('PMD[27] is a 2MB block mapping!');
    const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
    console.log(`Block PA: 0x${blockPA.toString(16)}`);
    console.log('This means 2MB of contiguous physical memory.');
}

fs.closeSync(fd);