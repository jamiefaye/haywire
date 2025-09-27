#!/usr/bin/env node
/**
 * Test the new branded types system
 */

import { VirtualAddress, VA } from './src/types/virtual-address.js';
import { PhysicalAddress, PA } from './src/types/physical-address.js';
import { PageTableEntry, walkPageTable } from './src/types/page-table.js';

console.log('Testing Branded Types System');
console.log('============================\n');

// Test Virtual Addresses
console.log('Virtual Address Tests:');
const va1 = VA('0xffff800000000000');
const va2 = VA(0x1234567890);
const va3 = VirtualAddress.from(12345n);

console.log(`  VA from string: ${VirtualAddress.toHex(va1)}`);
console.log(`  VA from number: ${VirtualAddress.toHex(va2)}`);
console.log(`  VA from bigint: ${VirtualAddress.toHex(va3)}`);

console.log(`\n  Is kernel? ${VirtualAddress.isKernel(va1)} (should be true)`);
console.log(`  Is user? ${VirtualAddress.isUser(va2)} (should be true)`);

const pgdIdx = VirtualAddress.pgdIndex(va1);
console.log(`  PGD index of ${VirtualAddress.toHex(va1)}: ${pgdIdx}`);

const va4 = VirtualAddress.add(va2, 0x1000);
console.log(`  Add 0x1000 to ${VirtualAddress.toHex(va2)}: ${VirtualAddress.toHex(va4)}`);

console.log(`  Region of ${VirtualAddress.toHex(va1)}: ${VirtualAddress.getRegion(va1)}`);

// Test Physical Addresses
console.log('\nPhysical Address Tests:');
const pa1 = PA('0x40000000');
const pa2 = PA(0x80000000);
const pa3 = PhysicalAddress.fromPageNumber(0x12345);

console.log(`  PA from string: ${PhysicalAddress.toHex(pa1)}`);
console.log(`  PA from number: ${PhysicalAddress.toHex(pa2)}`);
console.log(`  PA from page number: ${PhysicalAddress.toHex(pa3)}`);

console.log(`  Is in RAM? ${PhysicalAddress.isInRAM(pa1)} (should be true)`);
console.log(`  Page number: ${PhysicalAddress.pageNumber(pa3).toString(16)}`);

const pa4 = PhysicalAddress.pageAlign(PA(0x40001234));
console.log(`  Page align 0x40001234: ${PhysicalAddress.toHex(pa4)}`);

// Test Page Table Entries
console.log('\nPage Table Entry Tests:');
const pte1 = PageTableEntry.from(0x40000003n);
console.log(`  PTE 0x40000003:`);
console.log(`    Valid? ${PageTableEntry.isValid(pte1)}`);
console.log(`    Is Table? ${PageTableEntry.isTable(pte1)}`);
console.log(`    Physical Address: ${PhysicalAddress.toHex(PageTableEntry.getPhysicalAddress(pte1))}`);
console.log(`    Flags: ${PageTableEntry.getFlags(pte1).join(', ')}`);

// Test type safety (these should cause TypeScript errors if uncommented)
// const bad1 = va1 + pa1;  // Error: Can't add VA and PA
// const bad2 = VirtualAddress.pgdIndex(pa1);  // Error: Expected VA, got PA
// const bad3 = PhysicalAddress.isInRAM(va1);  // Error: Expected PA, got VA

// Test comparison
console.log('\nComparison Tests:');
const va5 = VA(0x1000);
const va6 = VA(0x2000);
console.log(`  Compare ${VirtualAddress.toHex(va5)} vs ${VirtualAddress.toHex(va6)}: ${VirtualAddress.compare(va5, va6)}`);

// Test decomposition
console.log('\nAddress Decomposition:');
const testVa = VA('0xc3048ea20840');
const parts = VirtualAddress.decompose(testVa);
console.log(`  VA ${VirtualAddress.toHex(testVa)} decomposes to:`);
console.log(`    PGD index: ${parts.pgd}`);
console.log(`    PUD index: ${parts.pud}`);
console.log(`    PMD index: ${parts.pmd}`);
console.log(`    PTE index: ${parts.pte}`);
console.log(`    Offset: 0x${parts.offset.toString(16)}`);
console.log(`    Is kernel? ${parts.isKernel}`);

console.log('\n✅ All tests completed successfully!');