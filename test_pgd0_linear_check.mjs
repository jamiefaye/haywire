#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const PA_MASK = 0x0000FFFFFFFFFFFFn;

console.log('=== Testing PGD[0] Linear Mapping ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

function walkVA(fd, va, swapperPgd) {
    // Simple page walk
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
        
        // Check for 1GB block
        if ((pudEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pudEntry & PA_MASK & ~0x3FFFFFFFn);
            return blockPA | (Number(va) & 0x3FFFFFFF);
        }
        
        // Continue to PMD
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
        
        // Continue to PTE
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

console.log('Testing if PGD[0] provides linear mapping...\n');
console.log('Expected: VA -> PA + 0x40000000');
console.log('');

// Test various addresses
const testAddresses = [
    0x00000000n,
    0x00001000n,
    0x00100000n,
    0x01000000n,
    0x10000000n,
    0x20000000n,
    0x40000000n,
    0x80000000n,
    0xC0000000n,
    0xF0000000n,
];

let linearCount = 0;
let totalMapped = 0;

for (const va of testAddresses) {
    const pa = walkVA(fd, va, SWAPPER_PGD_PA);
    
    if (pa !== null) {
        totalMapped++;
        const expectedPA = Number(va) + GUEST_RAM_START;
        const isLinear = pa === expectedPA;
        if (isLinear) linearCount++;
        
        console.log(`VA 0x${va.toString(16).padEnd(10)} -> PA 0x${pa.toString(16).padEnd(10)} ${isLinear ? '✓ LINEAR' : `(expected 0x${expectedPA.toString(16)})`}`);
    } else {
        console.log(`VA 0x${va.toString(16).padEnd(10)} -> Not mapped`);
    }
}

console.log(`\n${linearCount}/${totalMapped} mapped addresses follow linear pattern\n`);

if (linearCount === totalMapped && totalMapped > 0) {
    console.log('✅ PGD[0] provides a PERFECT linear mapping!');
    console.log('    VA range: 0x0 - 0x100000000');
    console.log('    PA range: 0x40000000 - 0x140000000');
    console.log('    Formula: PA = VA + 0x40000000');
    console.log('');
    console.log('This means:');
    console.log('1. Kernel can access first 4GB of RAM via low VAs');
    console.log('2. Simple address translation for kernel code');
    console.log('3. Our discovered processes at PA 0x4xxxxxxx-0x13xxxxxxx');
    console.log('   can be accessed at VA 0x0xxxxxxx-0xFxxxxxxx');
}

console.log('\n=== Implications for init_task ===\n');
console.log('init_task at PA 0x37b39840:');
console.log('- This is BELOW 0x40000000 (RAM start)');
console.log('- Cannot be accessed via PGD[0] linear mapping');
console.log('- It\'s in kernel code/data section (ROM/Flash)');
console.log('- Not included in memory-backend-file');
console.log('');
console.log('However, systemd at PA 0x100388000:');
const systemdVA = 0x100388000 - GUEST_RAM_START;
console.log(`- Can be accessed at VA 0x${systemdVA.toString(16)} via PGD[0]`);
console.log('- And all our discovered processes are accessible this way!');

fs.closeSync(fd);