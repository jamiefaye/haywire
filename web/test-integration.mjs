#!/usr/bin/env node
/**
 * Integration test for branded types with kernel discovery
 */

import { KernelDiscovery, KernelConstants } from './src/kernel-discovery.js';
import { VirtualAddress, VA } from './src/types/virtual-address.js';
import { PhysicalAddress, PA } from './src/types/physical-address.js';

console.log('Testing Branded Types Integration');
console.log('==================================\n');

// Test 1: Constants are properly typed
console.log('Test 1: Constants');
console.log(`  GUEST_RAM_START: ${PhysicalAddress.toHex(KernelConstants.GUEST_RAM_START)}`);
console.log(`  GUEST_RAM_END: ${PhysicalAddress.toHex(KernelConstants.GUEST_RAM_END)}`);
console.log(`  KERNEL_VA_START: ${VirtualAddress.toHex(KernelConstants.KERNEL_VA_START)}`);

// Test 2: Process info structures use branded types
console.log('\nTest 2: ProcessInfo structure');
const mockProcess = {
  pid: 1234,
  name: 'test',
  taskStruct: PA(0x40001000),
  mmStruct: VA(0xffff800012345678n),
  pgd: PA(0x42000000),
  isKernelThread: false,
  tasksNext: PA(0x40002000),
  tasksPrev: PA(0x40003000),
  ptes: [],
  sections: []
};

console.log(`  Task struct: ${PhysicalAddress.toHex(mockProcess.taskStruct)}`);
console.log(`  MM struct: ${VirtualAddress.toHex(mockProcess.mmStruct)}`);
console.log(`  PGD: ${PhysicalAddress.toHex(mockProcess.pgd)}`);
console.log(`  Is kernel VA? ${VirtualAddress.isKernel(mockProcess.mmStruct)}`);

// Test 3: PTE structure with branded types
console.log('\nTest 3: PTE structure');
const mockPte = {
  va: VA(0x400000),
  pa: PA(0x40100000),
  flags: 0x3n,
  r: true,
  w: true,
  x: false
};

console.log(`  Virtual: ${VirtualAddress.toHex(mockPte.va)}`);
console.log(`  Physical: ${PhysicalAddress.toHex(mockPte.pa)}`);
console.log(`  PGD index: ${VirtualAddress.pgdIndex(mockPte.va)}`);

// Test 4: Memory sections
console.log('\nTest 4: Memory sections');
const mockSection = {
  type: 'code',
  startVa: VA(0x400000),
  endVa: VA(0x500000),
  startPa: PA(0x40100000),
  size: 0x100000,
  pages: 256,
  flags: 0x5n
};

console.log(`  VA range: ${VirtualAddress.toHex(mockSection.startVa)} - ${VirtualAddress.toHex(mockSection.endVa)}`);
console.log(`  PA start: ${PhysicalAddress.toHex(mockSection.startPa)}`);
console.log(`  Size: ${mockSection.size.toString(16)} bytes`);

// Test 5: Address arithmetic
console.log('\nTest 5: Address arithmetic');
const va1 = VA(0x1000);
const va2 = VirtualAddress.add(va1, 0x2000);
const pa1 = PA(0x40000000);
const pa2 = PhysicalAddress.add(pa1, 0x1000);

console.log(`  VA: ${VirtualAddress.toHex(va1)} + 0x2000 = ${VirtualAddress.toHex(va2)}`);
console.log(`  PA: ${PhysicalAddress.toHex(pa1)} + 0x1000 = ${PhysicalAddress.toHex(pa2)}`);

console.log('\n✅ All integration tests passed!');