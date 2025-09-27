#!/usr/bin/env node
/**
 * Script to help convert codebase to use branded types
 * This script provides conversion utilities that can be imported and used
 */

import { VirtualAddress, VA } from './src/types/virtual-address';
import { PhysicalAddress, PA } from './src/types/physical-address';

/**
 * Conversion patterns to apply to TypeScript files
 */
export const conversionPatterns = [
  // Type declarations
  { from: /: number \| bigint;(\s*\/\/ Virtual address)/g, to: ': VirtualAddress;$1' },
  { from: /: number;(\s*\/\/ Physical address)/g, to: ': PhysicalAddress;$1' },
  { from: /pgd: number/g, to: 'pgd: PhysicalAddress' },
  { from: /taskStruct: number/g, to: 'taskStruct: PhysicalAddress' },
  { from: /mmStruct: bigint/g, to: 'mmStruct: VirtualAddress' },

  // Constants
  { from: /GUEST_RAM_START: (0x[0-9a-fA-F]+)/g, to: 'GUEST_RAM_START: PA($1)' },
  { from: /GUEST_RAM_END: (0x[0-9a-fA-F]+)/g, to: 'GUEST_RAM_END: PA($1)' },
  { from: /SWAPPER_PGD: (0x[0-9a-fA-F]+)/g, to: 'SWAPPER_PGD: PA($1)' },
  { from: /KERNEL_VA_START: (0x[0-9a-fA-F]+n)/g, to: 'KERNEL_VA_START: VA($1)' },

  // Method signatures
  { from: /translateVA\(va: number \| bigint\)/g, to: 'translateVA(va: VirtualAddress)' },
  { from: /walkPageTable\(tableAddr: number/g, to: 'walkPageTable(tableAddr: PhysicalAddress' },
  { from: /findSwapperPgDir\(\): number/g, to: 'findSwapperPgDir(): PhysicalAddress' },

  // Comparisons and operations
  { from: /\.pgd !== 0\)/g, to: '.pgd !== PA(0))' },
  { from: /= 0; \/\/ Track discovered swapper_pg_dir/g, to: '= PA(0); // Track discovered swapper_pg_dir' },

  // Map types
  { from: /Map<number, Set<number>>\(\); \/\/ page/g, to: 'Map<string, Set<number>>(); // page using string key for PA' },
  { from: /Set<number>\(\); \/\/ zero/g, to: 'Set<string>(); // zero pages using string for PA' },
];

/**
 * Helper to convert literal values in code
 */
export function convertLiteral(value: string, type: 'PA' | 'VA'): string {
  if (value.match(/^0x[0-9a-fA-F]+n?$/)) {
    return `${type}(${value})`;
  }
  return value;
}

/**
 * Convert address logging statements
 */
export function convertLogging(line: string): string {
  // Convert hex string interpolations
  line = line.replace(
    /0x\$\{([a-zA-Z_][a-zA-Z0-9_]*)(\.pgd)?\.toString\(16\)\}/g,
    (match, varName, prop) => {
      if (prop === '.pgd') {
        return `\${PhysicalAddress.toHex(${varName}.pgd)}`;
      }
      // Check context to determine if PA or VA
      if (varName.includes('pgd') || varName.includes('pa') || varName.includes('phys')) {
        return `\${PhysicalAddress.toHex(${varName})}`;
      }
      if (varName.includes('va') || varName.includes('virt') || varName.includes('mm')) {
        return `\${VirtualAddress.toHex(${varName})}`;
      }
      return match;
    }
  );

  return line;
}

/**
 * Key conversions that need to be made manually:
 *
 * 1. When using pageToPids Map:
 *    Old: if (!this.pageToPids.has(pte.pa)) {
 *    New: const paKey = PhysicalAddress.toHex(pte.pa);
 *         if (!this.pageToPids.has(paKey)) {
 *
 * 2. When comparing addresses:
 *    Old: if (va === currentSection.endVa)
 *    New: if (va === currentSection.endVa) // VirtualAddress supports === comparison
 *
 * 3. When doing arithmetic:
 *    Old: va + KernelConstants.PAGE_SIZE
 *    New: VirtualAddress.add(va, KernelConstants.PAGE_SIZE)
 *
 * 4. When checking ranges:
 *    Old: if (addr >= 0xffff000000000000n)
 *    New: if (VirtualAddress.isKernel(addr))
 *
 * 5. When creating from raw values:
 *    Old: const pa = Number(entry & PA_MASK);
 *    New: const pa = PA(entry & BigInt(KernelConstants.PA_MASK));
 */

console.log(`
Branded Types Conversion Helper
================================

This script contains patterns and utilities to help convert the codebase
to use branded types for Virtual and Physical addresses.

Key imports to add to files:
  import { VirtualAddress, VA } from './types/virtual-address';
  import { PhysicalAddress, PA } from './types/physical-address';
  import { PageTableEntry } from './types/page-table';

Run the actual conversion by:
1. Reviewing the patterns in this file
2. Testing on a single file first
3. Applying to all relevant TypeScript files

See the comments in this file for manual conversion patterns.
`);