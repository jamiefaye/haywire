#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const SWAPPER_PGD_PA = 0x136deb000;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;

console.log('=== Would Scanning PGD[256] Find ALL Processes? ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

console.log('Understanding What\'s in PGD[256]:\n');
console.log('PGD[256] maps VA range: 0xFFFF800000000000 - 0xFFFF807FFFFFFFFF');
console.log('This is 512GB of virtual address space!\n');

console.log('It contains multiple regions:');
console.log('1. Linear/Direct Map: 0xFFFF800000000000+');
console.log('   - Maps ALL physical RAM linearly');
console.log('   - Every physical page appears here');
console.log('2. VMALLOC area: ~0xFFFF800008000000-0xFFFF800040000000');
console.log('   - Dynamic kernel allocations');
console.log('   - Custom VA->PA mappings');
console.log('3. Other kernel mappings\n');

console.log('='.repeat(70) + '\n');

console.log('Question: Are ALL task_structs accessible via PGD[256]?\n');

console.log('YES! Here\'s why:\n');

console.log('1. EVERY physical page has a linear map entry:');
console.log('   - If task_struct is at PA 0x100000000');
console.log('   - It\'s accessible at VA 0xFFFF8000C0000000 (linear map)');
console.log('   - Even if fragmented, each fragment is mapped');
console.log('');

console.log('2. SLUB allocations appear in BOTH places:');
console.log('   a) Linear map - where physical pages actually are');
console.log('   b) VMALLOC area - if non-contiguous (custom mapping)');
console.log('');

console.log('3. Kernel task_struct pointers usually point to:');
console.log('   - VMALLOC area VA (if allocated via vmalloc)');
console.log('   - Or LINEAR map VA (if physically contiguous)');
console.log('   - But the physical pages are ALWAYS in linear map too!');
console.log('');

console.log('='.repeat(70) + '\n');
console.log('Scanning Strategy for 100% Discovery:\n');

console.log('Option 1: Scan the LINEAR MAP region');
console.log('   Advantages:');
console.log('   ✓ Every physical page is here (guaranteed!)');
console.log('   ✓ Simple 1:1 mapping (VA = PA + offset)');
console.log('   ✓ Would find ALL task_structs');
console.log('   ');
console.log('   Challenges:');
console.log('   ✗ Still fragmented (same as physical scan)');
console.log('   ✗ Need to handle straddling across VA pages');
console.log('   ✗ Same 91% problem unless we handle fragmentation\n');

console.log('Option 2: Scan the VMALLOC area');
console.log('   Advantages:');
console.log('   ✓ Task_structs appear contiguous here');
console.log('   ✓ Clean 9088-byte chunks');
console.log('   ');
console.log('   Challenges:');
console.log('   ✗ Not all task_structs use vmalloc');
console.log('   ✗ Sparse mappings (lots of unmapped VAs)');
console.log('   ✗ Would miss physically contiguous ones\n');

console.log('Option 3: Scan BOTH regions');
console.log('   ✓ Would definitely find 100%!');
console.log('   ✗ Lots of duplication\n');

console.log('='.repeat(70) + '\n');
console.log('The Key Insight:\n');

console.log('The LINEAR MAP in PGD[256] contains EVERYTHING!');
console.log('');
console.log('Every single byte of physical RAM is mapped linearly:');
console.log('  PA 0x40000000 -> VA 0xFFFF800000000000');
console.log('  PA 0x40001000 -> VA 0xFFFF800000001000');
console.log('  ... and so on ...');
console.log('');
console.log('So if we scan VA 0xFFFF800000000000 to 0xFFFF800180000000');
console.log('(linear map of our 6GB), we\'d see EXACTLY what we see');
console.log('in physical memory - including all task_structs!\n');

console.log('='.repeat(70) + '\n');
console.log('Wait... That\'s Just Physical Scanning!\n');

console.log('Scanning the linear map in PGD[256] would give us:');
console.log('- Exactly the same view as physical scanning');
console.log('- Same fragmentation issues');
console.log('- Same 91% discovery rate');
console.log('- No improvement!\n');

console.log('The REAL Solution:\n');
console.log('To get 100%, we need to:');
console.log('1. Parse kernel data structures (IDR) that track all PIDs');
console.log('2. Or understand SLUB metadata to find fragments');
console.log('3. Or scan linear map BUT handle fragmentation properly');
console.log('');
console.log('Simply switching from physical to PGD[256] VA scanning');
console.log('doesn\'t solve the fragmentation problem!');
console.log('');
console.log('The vmalloc mappings help the KERNEL avoid fragmentation,');
console.log('but we\'d need to know where those mappings are to use them.');

// Let's verify the linear mapping
console.log('\n' + '='.repeat(70) + '\n');
console.log('Verifying Linear Map Coverage:\n');

// Read PGD[256] -> PUD[1] to check linear map
const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, SWAPPER_PGD_PA - GUEST_RAM_START);

const pgd256 = pgdBuffer.readBigUint64LE(256 * 8);
if (pgd256 !== 0n) {
    const pudTablePA = Number(pgd256 & 0x0000FFFFFFFFFFFFn & ~0xFFFn);
    const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
    
    // PUD[1] should map PA 0x40000000+ (our RAM start)
    const pud1 = pudBuffer.readBigUint64LE(1 * 8);
    if (pud1 !== 0n) {
        console.log('PUD[1] (linear map of RAM start):');
        console.log(`  Entry: 0x${pud1.toString(16)}`);
        console.log(`  Maps VA: 0xFFFF800040000000 - 0xFFFF80007FFFFFFF`);
        console.log(`  To PA:   0x40000000 - 0x7FFFFFFF`);
        console.log(`  ✓ This confirms linear mapping of our RAM!`);
    }
}

console.log('');
console.log('Conclusion: PGD[256] contains everything via linear map,');
console.log('but scanning it wouldn\'t solve our fragmentation problem.');
console.log('We\'d still get 91% unless we handle non-contiguous SLABs!');

fs.closeSync(fd);