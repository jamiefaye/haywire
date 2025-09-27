/**
 * Page Table Entry types and operations for ARM64
 */

import { PhysicalAddress, PA } from './physical-address';
import { VirtualAddress, VA } from './virtual-address';

// Brand for Page Table Entry type safety
declare const PTE_BRAND: unique symbol;
declare const PGD_BRAND: unique symbol;
declare const PUD_BRAND: unique symbol;
declare const PMD_BRAND: unique symbol;

/**
 * Page Table Entry types with branding for type safety
 */
export type PGDEntry = bigint & { [PGD_BRAND]: void };
export type PUDEntry = bigint & { [PUD_BRAND]: void };
export type PMDEntry = bigint & { [PMD_BRAND]: void };
export type PTEEntry = bigint & { [PTE_BRAND]: void };

// Generic page table entry type
export type PageTableEntry = PGDEntry | PUDEntry | PMDEntry | PTEEntry;

/**
 * ARM64 Page Table Entry Flags
 */
export const PTE_FLAGS = {
  VALID: 1n << 0n,           // Entry is valid
  TABLE: 1n << 1n,           // Points to next level table (not a block)

  // Access permissions
  AP_RO: 1n << 7n,           // Read-only (AP[2])
  AP_EL0: 1n << 6n,          // Accessible from EL0 (AP[1])

  // Memory attributes
  ATTR_NORMAL: 0n,           // Normal memory
  ATTR_DEVICE: 1n,           // Device memory

  // Shareability
  SH_NONE: 0n << 8n,         // Non-shareable
  SH_OUTER: 2n << 8n,        // Outer shareable
  SH_INNER: 3n << 8n,        // Inner shareable

  // Access flag
  AF: 1n << 10n,             // Access flag (set on access)

  // Not Global
  nG: 1n << 11n,             // Not global (ASID-specific)

  // Dirty bit
  DBM: 1n << 51n,            // Dirty bit modifier

  // Contiguous hint
  CONT: 1n << 52n,           // Contiguous hint

  // Privileged execute never
  PXN: 1n << 53n,            // Privileged execute never

  // Execute never
  UXN: 1n << 54n,            // User execute never
} as const;

/**
 * Page Table Entry operations namespace
 */
export namespace PageTableEntry {
  /**
   * Create a page table entry from raw value
   */
  export function from(value: bigint | number): PageTableEntry {
    return BigInt(value) as PageTableEntry;
  }

  /**
   * Check if entry is valid
   */
  export function isValid(entry: PageTableEntry): boolean {
    return (entry & PTE_FLAGS.VALID) !== 0n;
  }

  /**
   * Check if entry points to a table (not a block/page)
   */
  export function isTable(entry: PageTableEntry): boolean {
    return isValid(entry) && (entry & PTE_FLAGS.TABLE) !== 0n;
  }

  /**
   * Check if entry is a block/page mapping
   */
  export function isBlock(entry: PageTableEntry): boolean {
    return isValid(entry) && (entry & PTE_FLAGS.TABLE) === 0n;
  }

  /**
   * Extract physical address from entry
   */
  export function getPhysicalAddress(entry: PageTableEntry): PhysicalAddress {
    // ARM64: PA is in bits 47:12
    return PA(entry & 0x0000FFFFFFFFF000n);
  }

  /**
   * Extract page frame number (PFN)
   */
  export function getPFN(entry: PageTableEntry): bigint {
    return PhysicalAddress.pageNumber(getPhysicalAddress(entry));
  }

  /**
   * Check if entry is writable
   */
  export function isWritable(entry: PageTableEntry): boolean {
    return isValid(entry) && (entry & PTE_FLAGS.AP_RO) === 0n;
  }

  /**
   * Check if entry is user accessible
   */
  export function isUserAccessible(entry: PageTableEntry): boolean {
    return isValid(entry) && (entry & PTE_FLAGS.AP_EL0) !== 0n;
  }

  /**
   * Check if entry is executable
   */
  export function isExecutable(entry: PageTableEntry): boolean {
    return isValid(entry) && (entry & PTE_FLAGS.UXN) === 0n;
  }

  /**
   * Get human-readable flags description
   */
  export function getFlags(entry: PageTableEntry): string[] {
    const flags: string[] = [];

    if (!isValid(entry)) {
      return ['INVALID'];
    }

    if (isTable(entry)) {
      flags.push('TABLE');
    } else {
      flags.push('PAGE');
    }

    if (isWritable(entry)) {
      flags.push('W');
    } else {
      flags.push('R');
    }

    if (isUserAccessible(entry)) {
      flags.push('U');
    }

    if (isExecutable(entry)) {
      flags.push('X');
    }

    if (entry & PTE_FLAGS.AF) {
      flags.push('ACCESSED');
    }

    if (entry & PTE_FLAGS.CONT) {
      flags.push('CONT');
    }

    return flags;
  }

  /**
   * Format entry as hex string
   */
  export function toHex(entry: PageTableEntry): string {
    return `0x${entry.toString(16).padStart(16, '0')}`;
  }

  /**
   * Create a page table entry from physical address and flags
   */
  export function create(pa: PhysicalAddress, flags: bigint = PTE_FLAGS.VALID): PageTableEntry {
    return ((pa & 0x0000FFFFFFFFF000n) | flags) as PageTableEntry;
  }
}

/**
 * Page Table Walker configuration
 */
export interface PageTableConfig {
  pgdBase: PhysicalAddress;
  readMemory: (pa: PhysicalAddress, size: number) => Promise<ArrayBuffer>;
}

/**
 * Result of a page table walk
 */
export interface PageWalkResult {
  valid: boolean;
  physicalAddress?: PhysicalAddress;
  pageSize: number;
  flags: string[];
  level: 'PGD' | 'PUD' | 'PMD' | 'PTE';
  entries: {
    pgd?: PGDEntry;
    pud?: PUDEntry;
    pmd?: PMDEntry;
    pte?: PTEEntry;
  };
}

/**
 * Walk page tables for a virtual address
 */
export async function walkPageTable(
  va: VirtualAddress,
  config: PageTableConfig
): Promise<PageWalkResult> {
  const indices = VirtualAddress.decompose(va);
  const result: PageWalkResult = {
    valid: false,
    pageSize: 4096,
    flags: [],
    level: 'PGD',
    entries: {}
  };

  try {
    // Read PGD entry
    const pgdAddr = PhysicalAddress.add(config.pgdBase, indices.pgd * 8);
    const pgdData = await config.readMemory(pgdAddr, 8);
    const pgdEntry = new DataView(pgdData).getBigUint64(0, true) as PGDEntry;
    result.entries.pgd = pgdEntry;

    if (!PageTableEntry.isValid(pgdEntry)) {
      return result;
    }

    if (!PageTableEntry.isTable(pgdEntry)) {
      // 1GB page at PGD level
      result.valid = true;
      result.physicalAddress = PageTableEntry.getPhysicalAddress(pgdEntry);
      result.pageSize = 1024 * 1024 * 1024;
      result.level = 'PGD';
      result.flags = PageTableEntry.getFlags(pgdEntry);
      return result;
    }

    // Read PUD entry
    const pudBase = PageTableEntry.getPhysicalAddress(pgdEntry);
    const pudAddr = PhysicalAddress.add(pudBase, indices.pud * 8);
    const pudData = await config.readMemory(pudAddr, 8);
    const pudEntry = new DataView(pudData).getBigUint64(0, true) as PUDEntry;
    result.entries.pud = pudEntry;
    result.level = 'PUD';

    if (!PageTableEntry.isValid(pudEntry)) {
      return result;
    }

    if (!PageTableEntry.isTable(pudEntry)) {
      // 2MB page at PUD level
      result.valid = true;
      result.physicalAddress = PageTableEntry.getPhysicalAddress(pudEntry);
      result.pageSize = 2 * 1024 * 1024;
      result.flags = PageTableEntry.getFlags(pudEntry);
      return result;
    }

    // Read PMD entry
    const pmdBase = PageTableEntry.getPhysicalAddress(pudEntry);
    const pmdAddr = PhysicalAddress.add(pmdBase, indices.pmd * 8);
    const pmdData = await config.readMemory(pmdAddr, 8);
    const pmdEntry = new DataView(pmdData).getBigUint64(0, true) as PMDEntry;
    result.entries.pmd = pmdEntry;
    result.level = 'PMD';

    if (!PageTableEntry.isValid(pmdEntry)) {
      return result;
    }

    if (!PageTableEntry.isTable(pmdEntry)) {
      // 2MB page at PMD level
      result.valid = true;
      result.physicalAddress = PageTableEntry.getPhysicalAddress(pmdEntry);
      result.pageSize = 2 * 1024 * 1024;
      result.flags = PageTableEntry.getFlags(pmdEntry);
      return result;
    }

    // Read PTE entry
    const pteBase = PageTableEntry.getPhysicalAddress(pmdEntry);
    const pteAddr = PhysicalAddress.add(pteBase, indices.pte * 8);
    const pteData = await config.readMemory(pteAddr, 8);
    const pteEntry = new DataView(pteData).getBigUint64(0, true) as PTEEntry;
    result.entries.pte = pteEntry;
    result.level = 'PTE';

    if (!PageTableEntry.isValid(pteEntry)) {
      return result;
    }

    // 4KB page
    result.valid = true;
    result.physicalAddress = PhysicalAddress.add(
      PageTableEntry.getPhysicalAddress(pteEntry),
      indices.offset
    );
    result.pageSize = 4096;
    result.flags = PageTableEntry.getFlags(pteEntry);
    return result;

  } catch (error) {
    console.error('Page walk failed:', error);
    return result;
  }
}

// Export convenience functions
export function PGD(value: bigint | number): PGDEntry {
  return PageTableEntry.from(value) as PGDEntry;
}

export function PUD(value: bigint | number): PUDEntry {
  return PageTableEntry.from(value) as PUDEntry;
}

export function PMD(value: bigint | number): PMDEntry {
  return PageTableEntry.from(value) as PMDEntry;
}

export function PTE(value: bigint | number): PTEEntry {
  return PageTableEntry.from(value) as PTEEntry;
}