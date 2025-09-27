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

console.log('=== Scanning VMALLOC Regions for Task Structs ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Function to walk page tables and translate VA to PA
function translateVA(fd, va, swapperPgd) {
    const pgdIndex = Number((va >> 39n) & 0x1FFn);
    const pudIndex = Number((va >> 30n) & 0x1FFn);
    const pmdIndex = Number((va >> 21n) & 0x1FFn);
    const pteIndex = Number((va >> 12n) & 0x1FFn);
    const pageOffset = Number(va & 0xFFFn);
    
    try {
        // Read PGD
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, swapperPgd - GUEST_RAM_START);
        const pgdEntry = pgdBuffer.readBigUint64LE(pgdIndex * 8);
        if ((pgdEntry & 0x3n) === 0n) return null;
        
        // Read PUD
        const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        const pudEntry = pudBuffer.readBigUint64LE(pudIndex * 8);
        if ((pudEntry & 0x3n) === 0n) return null;
        
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

// Function to check if a VA contains a task_struct
function checkForTaskStruct(fd, va, swapperPgd) {
    // Translate VA to PA
    const pa = translateVA(fd, va, swapperPgd);
    if (!pa) return null;
    
    const offset = pa - GUEST_RAM_START;
    if (offset < 0 || offset + TASK_STRUCT_SIZE > fs.fstatSync(fd).size) return null;
    
    try {
        // Read potential PID
        const pidBuffer = Buffer.allocUnsafe(4);
        fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
        const pid = pidBuffer.readUint32LE(0);
        
        if (pid < 0 || pid > 32768) return null;
        
        // Read potential comm
        const commBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
        const comm = commBuffer.toString('ascii').split('\0')[0];
        
        if (!comm || comm.length === 0 || comm.length > 15) return null;
        if (!/^[\x20-\x7E]+$/.test(comm)) return null;
        
        return { va: va.toString(16), pa, pid, comm };
    } catch (e) {
        return null;
    }
}

console.log('Scanning non-linear mapped regions in PGD[256]...\n');

// We found non-linear mappings at:
// VA 0xFFFF800080000000+ with scattered PAs

const foundTasks = [];
const vmalloc_regions = [
    { start: 0xFFFF800080000000n, size: 0x8000000n },  // 128MB region
    { start: 0xFFFF800040000000n, size: 0x8000000n },  // Another region
];

console.log('Checking vmalloc regions for task_struct signatures...\n');

for (const region of vmalloc_regions) {
    console.log(`Scanning VA 0x${region.start.toString(16)} - 0x${(region.start + region.size).toString(16)}`);
    
    let checked = 0;
    let found = 0;
    
    // Check every 16KB (potential task_struct allocation)
    for (let va = region.start; va < region.start + region.size; va += 0x4000n) {
        const result = checkForTaskStruct(fd, va, SWAPPER_PGD_PA);
        if (result) {
            found++;
            foundTasks.push(result);
            if (found <= 5) {
                console.log(`  Found: PID ${result.pid} "${result.comm}" at VA 0x${result.va}`);
            }
        }
        checked++;
        
        if (checked % 1000 === 0) {
            process.stdout.write(`  Checked ${checked} VAs, found ${found} task_structs\r`);
        }
    }
    console.log(`  Checked ${checked} VAs, found ${found} task_structs`);
    console.log('');
}

console.log(`\nTotal task_structs found in vmalloc regions: ${foundTasks.length}\n`);

if (foundTasks.length > 0) {
    console.log('Sample of found processes:');
    for (const task of foundTasks.slice(0, 10)) {
        console.log(`  PID ${task.pid}: ${task.comm} at VA 0x${task.va}`);
    }
    
    // Check if these are ones we were missing
    console.log('\nThese could be the missing 9%!');
    console.log('They\'re in vmalloc space with contiguous VA mappings.');
    console.log('The physical pages are fragmented but appear contiguous here!');
} else {
    console.log('No clear task_structs found in vmalloc regions.');
    console.log('');
    console.log('Possible reasons:');
    console.log('1. Task_structs might use different VA ranges');
    console.log('2. Most might be in physically contiguous allocations');
    console.log('3. Need to scan more comprehensively');
}

console.log('\n' + '='.repeat(70) + '\n');
console.log('The Complete Picture:\n');
console.log('ALL task_structs exist in one or both places:');
console.log('');
console.log('1. Linear map (always) - VA = PA + offset');
console.log('   - Every physical page is here');
console.log('   - Fragmented if physical is fragmented');
console.log('   - This is what we scan (91% success)');
console.log('');
console.log('2. VMALLOC area (sometimes) - custom VA->PA mappings');
console.log('   - Only for non-contiguous allocations');
console.log('   - Makes fragments appear contiguous');
console.log('   - Kernel pointers point here');
console.log('   - The missing 9% are accessible here!');
console.log('');
console.log('To get 100%:');
console.log('- Scan linear map (current approach) = 91%');
console.log('- PLUS scan vmalloc mappings = remaining 9%');
console.log('- OR parse IDR which has pointers to both!');

fs.closeSync(fd);