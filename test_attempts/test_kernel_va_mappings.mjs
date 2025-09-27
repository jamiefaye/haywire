#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;

console.log('=== Where Kernel VA Mappings Actually Live ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Read swapper_pg_dir
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);

console.log('Kernel Virtual Address Ranges (ARM64):\n');
console.log('PGD[0]:   VA 0x0000000000000000 - 0x0000007FFFFFFFFF (user space)');
console.log('PGD[256]: VA 0xFFFF800000000000 - 0xFFFF807FFFFFFFFF (kernel linear map)');
console.log('PGD[507]: VA 0xFFFFFD8000000000 - 0xFFFFFDFFFFFFFFFF (fixmap/special)');
console.log('PGD[511]: VA 0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF (kernel modules)');
console.log('');
console.log('='.repeat(70) + '\n');

console.log('How Each Region Handles Memory:\n');

console.log('1. PGD[256] - LINEAR/DIRECT MAP (0xFFFF800000000000+)');
console.log('   ' + '-'.repeat(50));
console.log('   Purpose: Direct mapping of ALL physical RAM');
console.log('   Formula: VA = PA + 0xFFFF800000000000 - 0x40000000');
console.log('   ');
console.log('   Example: PA 0x100000000 -> VA 0xFFFF8000C0000000');
console.log('   ');
console.log('   This is a SIMPLE 1:1 mapping!');
console.log('   - Every physical page has a corresponding VA here');
console.log('   - But STILL fragmented if physical is fragmented');
console.log('   - NOT where "clean" task_struct views come from!');
console.log('');

console.log('2. PGD[256] - VMALLOC AREA (also in PGD[256], higher addresses)');
console.log('   ' + '-'.repeat(50));
console.log('   VA Range: Typically 0xFFFF800008000000 - 0xFFFF800040000000');
console.log('   Purpose: Dynamic kernel allocations');
console.log('   ');
console.log('   THIS IS WHERE THE MAGIC HAPPENS!');
console.log('   - vmalloc() allocates virtually contiguous memory');
console.log('   - Can map scattered physical pages to contiguous VAs');
console.log('   - SLUB/SLAB allocators use this for large allocations');
console.log('   ');
console.log('   When SLUB gets non-contiguous physical pages:');
console.log('   1. It maps them to CONTIGUOUS virtual addresses here');
console.log('   2. Returns the VA pointer to the kernel');
console.log('   3. Kernel sees clean, contiguous memory!');
console.log('');

console.log('3. PGD[507] - FIXMAP (0xFFFFFD8000000000+)');
console.log('   ' + '-'.repeat(50));
console.log('   Purpose: Fixed VA mappings for special purposes');
console.log('   - Early boot console');
console.log('   - Atomic kmaps');
console.log('   - NOT for regular allocations');
console.log('');

console.log('4. PGD[511] - KERNEL MODULES (0xFFFFFF8000000000+)');
console.log('   ' + '-'.repeat(50));
console.log('   Purpose: Loadable kernel modules');
console.log('   - Module code and data');
console.log('   - Also uses vmalloc-style mappings');
console.log('');

console.log('='.repeat(70) + '\n');
console.log('The Key Insight: VMALLOC AREA in PGD[256]\n');

console.log('When you allocate a task_struct:');
console.log('');
console.log('1. SLUB tries to get contiguous physical pages (order-2, 16KB)');
console.log('');
console.log('2. If that fails (memory fragmentation), it gets individual pages:');
console.log('   - Page A at PA 0x100000000');
console.log('   - Page B at PA 0x123456000');
console.log('   - Page C at PA 0x198765000');
console.log('');
console.log('3. SLUB creates a VMALLOC mapping:');
console.log('   - Allocates VA range: 0xFFFF800012345000 - 0xFFFF800012347380');
console.log('   - Maps: VA+0x0000 -> PA 0x100000000');
console.log('   - Maps: VA+0x1000 -> PA 0x123456000');
console.log('   - Maps: VA+0x2000 -> PA 0x198765000');
console.log('');
console.log('4. Returns VA 0xFFFF800012345000 as the task_struct pointer');
console.log('');
console.log('5. Kernel code just uses task->pid, task->comm normally!');
console.log('   The MMU transparently handles the VA->PA translation.');
console.log('');
console.log('='.repeat(70) + '\n');
console.log('Checking Our PGD[256] Mappings:\n');

// Let's check what's actually in PGD[256]
const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
console.log(`PGD[256] entry: 0x${pgd256.toString(16)}`);

if (pgd256 !== 0n) {
    const pudTablePA = Number(pgd256 & 0x0000FFFFFFFFFFFFn & ~0xFFFn);
    console.log(`Points to PUD table at PA: 0x${pudTablePA.toString(16)}`);
    
    // Read some PUD entries
    const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
    
    console.log('\nSample PUD entries (first few):');
    for (let i = 0; i < 4; i++) {
        const pudEntry = pudBuffer.readBigUint64LE(i * 8);
        if (pudEntry !== 0n) {
            const vaStart = 0xFFFF800000000000n + (BigInt(i) << 30n);
            console.log(`  PUD[${i}]: VA 0x${vaStart.toString(16)} = 0x${pudEntry.toString(16)}`);
        }
    }
}

console.log('');
console.log('='.repeat(70) + '\n');
console.log('Summary:\n');
console.log('• The "clean" contiguous view is in the VMALLOC area (PGD[256])');
console.log('• Linear map (also PGD[256]) just mirrors physical layout');
console.log('• SLUB/vmalloc creates custom mappings for fragmented allocations');
console.log('• Kernel pointers are VAs in this vmalloc space');
console.log('• This is why we miss 9% - they\'re fragmented physically!');
console.log('');
console.log('To find the missing 9%, we\'d need to:');
console.log('1. Walk the vmalloc area page tables to find all mappings');
console.log('2. Or parse SLUB metadata to find all allocated pages');
console.log('3. Or follow kernel structures (IDR) that use these VAs');

fs.closeSync(fd);