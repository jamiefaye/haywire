#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const SLAB_SIZE = 0x8000; // 32KB SLAB
const SLAB_OFFSETS = [0x0, 0x2380, 0x4700]; // 3 task_structs per SLAB

console.log('=== Straddle-Aware Task_Struct Scanner ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Function to translate VA to PA using page tables
function translateVAtoPA(fd, va, pgdPA) {
    const pgdIndex = Number((va >> 39n) & 0x1FFn);
    const pudIndex = Number((va >> 30n) & 0x1FFn);
    const pmdIndex = Number((va >> 21n) & 0x1FFn);
    const pteIndex = Number((va >> 12n) & 0x1FFn);
    const pageOffset = Number(va & 0xFFFn);
    
    try {
        // Read PGD
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, pgdPA - GUEST_RAM_START);
        const pgdEntry = pgdBuffer.readBigUint64LE(pgdIndex * 8);
        if ((pgdEntry & 0x3n) === 0n) return null;
        
        // Read PUD
        const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        const pudEntry = pudBuffer.readBigUint64LE(pudIndex * 8);
        if ((pudEntry & 0x3n) === 0n) return null;
        
        // Check for 1GB block
        if ((pudEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
            return blockPA | (Number(va) & 0x3FFFFFFF);
        }
        
        // Read PMD
        const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        const pmdEntry = pmdBuffer.readBigUint64LE(pmdIndex * 8);
        if ((pmdEntry & 0x3n) === 0n) return null;
        
        // Check for 2MB block
        if ((pmdEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
            return blockPA | (Number(va) & 0x1FFFFF);
        }
        
        // Read PTE
        const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        const pteEntry = pteBuffer.readBigUint64LE(pteIndex * 8);
        if ((pteEntry & 0x3n) === 0n) return null;
        
        const pagePA = Number(pteEntry & PA_MASK & ~0xFFFn);
        return pagePA | pageOffset;
        
    } catch (e) {
        return null;
    }
}

// Check if a given PA looks like a SLAB page with task_structs
function checkIfSLABPage(fd, pa) {
    const offset = pa - GUEST_RAM_START;
    if (offset < 0 || offset + PAGE_SIZE > fs.fstatSync(fd).size) {
        return false;
    }
    
    // Quick check: does offset 0x750 (PID) look valid?
    const pidBuffer = Buffer.allocUnsafe(4);
    try {
        fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
        const pid = pidBuffer.readUint32LE(0);
        if (pid > 0 && pid < 32768) {
            return true;
        }
    } catch (e) {}
    
    return false;
}

// Read task_struct handling page straddling via kernel VA
function readTaskStructViaVA(fd, slabPA, slabOffset) {
    // For linear map, VA = PA - 0x40000000 + 0xFFFF800040000000
    const slabVA = BigInt(slabPA - GUEST_RAM_START) + 0xFFFF800040000000n;
    const taskVA = slabVA + BigInt(slabOffset);
    
    console.log(`  Reading task at SLAB offset 0x${slabOffset.toString(16)}`);
    console.log(`    VA: 0x${taskVA.toString(16)}`);
    
    const taskData = Buffer.allocUnsafe(TASK_STRUCT_SIZE);
    
    // Read page by page, using VA to find each page's PA
    for (let pageIdx = 0; pageIdx < 3; pageIdx++) {
        const pageVA = (taskVA & ~0xFFFn) + BigInt(pageIdx * PAGE_SIZE);
        const pagePA = translateVAtoPA(fd, pageVA, SWAPPER_PGD_PA);
        
        if (!pagePA) {
            console.log(`    Failed to translate VA 0x${pageVA.toString(16)}`);
            return null;
        }
        
        console.log(`    Page ${pageIdx}: VA 0x${pageVA.toString(16)} -> PA 0x${pagePA.toString(16)}`);
        
        // Calculate how much of the task_struct is in this page
        const taskStart = Number(taskVA);
        const pageStart = Number(pageVA);
        const pageEnd = pageStart + PAGE_SIZE;
        
        const readStart = Math.max(taskStart, pageStart);
        const readEnd = Math.min(taskStart + TASK_STRUCT_SIZE, pageEnd);
        
        if (readEnd > readStart) {
            const bufferOffset = readStart - taskStart;
            const fileOffset = pagePA - GUEST_RAM_START + (readStart - pageStart);
            const readSize = readEnd - readStart;
            
            try {
                fs.readSync(fd, taskData, bufferOffset, readSize, fileOffset);
            } catch (e) {
                console.log(`    Failed to read PA 0x${pagePA.toString(16)}`);
                return null;
            }
        }
    }
    
    // Now we have the complete task_struct, check if valid
    const pid = taskData.readUint32LE(PID_OFFSET);
    const comm = taskData.subarray(COMM_OFFSET, COMM_OFFSET + 16).toString('ascii').split('\0')[0];
    
    if (pid > 0 && pid < 32768 && comm && /^[\x20-\x7E]+$/.test(comm)) {
        console.log(`    ✓ Found: PID ${pid} "${comm}"`);
        return { pid, comm, va: taskVA };
    }
    
    return null;
}

console.log('Scanning for SLAB pages containing task_structs...\n');

const foundTasks = [];
const checkedPAs = new Set();

// Scan physical memory for SLAB pages
for (let pa = GUEST_RAM_START; pa < GUEST_RAM_START + 0x180000000; pa += SLAB_SIZE) {
    if (checkedPAs.has(pa)) continue;
    
    if (checkIfSLABPage(fd, pa)) {
        console.log(`Found SLAB at PA 0x${pa.toString(16)}`);
        checkedPAs.add(pa);
        
        // Try to read all 3 task_structs in this SLAB
        for (const offset of SLAB_OFFSETS) {
            const task = readTaskStructViaVA(fd, pa, offset);
            if (task) {
                foundTasks.push(task);
            }
        }
        console.log('');
    }
    
    if (foundTasks.length > 20) {
        console.log('(Stopping after 20 tasks for demo)\n');
        break;
    }
}

console.log('='.repeat(70) + '\n');
console.log(`Found ${foundTasks.length} tasks using VA-aware straddling:\n`);

for (const task of foundTasks) {
    console.log(`  PID ${task.pid}: ${task.comm}`);
}

console.log('\n' + '='.repeat(70) + '\n');
console.log('Key Insight:');
console.log('-----------');
console.log('By using kernel VA mappings to handle page straddling:');
console.log('1. We can follow non-contiguous SLAB pages');
console.log('2. Task at offset 0x4700 can span 3 different physical pages');
console.log('3. The VA mappings tell us where each page is');
console.log('4. This should find the missing 9%!');

fs.closeSync(fd);