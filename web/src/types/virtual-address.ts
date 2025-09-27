/**
 * Virtual Address type system using branded types with bigint
 * Provides type safety while maintaining performance
 */

// Brand for Virtual Address type safety
declare const VA_BRAND: unique symbol;

/**
 * Virtual Address type - a branded bigint that represents a 64-bit virtual address
 * This ensures type safety and prevents mixing virtual addresses with regular numbers
 */
export type VirtualAddress = bigint & { [VA_BRAND]: void };

/**
 * Namespace for Virtual Address operations
 * All VA manipulation should go through these functions for type safety
 */
export namespace VirtualAddress {
  /**
   * Create a VirtualAddress from various input types
   */
  export function from(value: bigint | number | string): VirtualAddress {
    if (typeof value === 'string') {
      // Handle hex strings
      if (value.startsWith('0x') || value.startsWith('0X')) {
        return BigInt(value) as VirtualAddress;
      }
      // Handle decimal strings
      return BigInt(value) as VirtualAddress;
    }
    if (typeof value === 'number') {
      return BigInt(value) as VirtualAddress;
    }
    return value as VirtualAddress;
  }

  /**
   * Create a VirtualAddress from a physical address (identity mapping for now)
   */
  export function fromPhysical(pa: bigint): VirtualAddress {
    // In the future, this might apply kernel VA offset transformations
    return pa as VirtualAddress;
  }

  /**
   * Zero virtual address constant
   */
  export const ZERO: VirtualAddress = 0n as VirtualAddress;

  /**
   * Kernel space start address (ARM64)
   */
  export const KERNEL_START: VirtualAddress = 0xFFFF000000000000n as VirtualAddress;

  /**
   * Check if address is in kernel space (top 16 bits set)
   */
  export function isKernel(va: VirtualAddress): boolean {
    return (va & 0xFFFF000000000000n) === 0xFFFF000000000000n;
  }

  /**
   * Check if address is in user space
   */
  export function isUser(va: VirtualAddress): boolean {
    return !isKernel(va);
  }

  /**
   * Extract PGD index (bits 47:39)
   */
  export function pgdIndex(va: VirtualAddress): number {
    return Number((va >> 39n) & 0x1FFn);
  }

  /**
   * Extract PUD index (bits 38:30)
   */
  export function pudIndex(va: VirtualAddress): number {
    return Number((va >> 30n) & 0x1FFn);
  }

  /**
   * Extract PMD index (bits 29:21)
   */
  export function pmdIndex(va: VirtualAddress): number {
    return Number((va >> 21n) & 0x1FFn);
  }

  /**
   * Extract PTE index (bits 20:12)
   */
  export function pteIndex(va: VirtualAddress): number {
    return Number((va >> 12n) & 0x1FFn);
  }

  /**
   * Extract page offset (bits 11:0)
   */
  export function pageOffset(va: VirtualAddress): number {
    return Number(va & 0xFFFn);
  }

  /**
   * Get the page-aligned address (clear bottom 12 bits)
   */
  export function pageAlign(va: VirtualAddress): VirtualAddress {
    return ((va >> 12n) << 12n) as VirtualAddress;
  }

  /**
   * Add offset to virtual address
   */
  export function add(va: VirtualAddress, offset: bigint | number): VirtualAddress {
    return (va + BigInt(offset)) as VirtualAddress;
  }

  /**
   * Subtract from virtual address
   */
  export function subtract(va: VirtualAddress, offset: bigint | number): VirtualAddress {
    return (va - BigInt(offset)) as VirtualAddress;
  }

  /**
   * Calculate distance between two virtual addresses
   */
  export function distance(va1: VirtualAddress, va2: VirtualAddress): bigint {
    return va1 > va2 ? va1 - va2 : va2 - va1;
  }

  /**
   * Format as hex string with 0x prefix
   */
  export function toHex(va: VirtualAddress): string {
    return `0x${va.toString(16).padStart(16, '0')}`;
  }

  /**
   * Format as short hex (no leading zeros)
   */
  export function toShortHex(va: VirtualAddress): string {
    return `0x${va.toString(16)}`;
  }

  /**
   * Convert to regular bigint (removes branding)
   */
  export function toBigInt(va: VirtualAddress): bigint {
    return va;
  }

  /**
   * Convert to number (WARNING: loses precision above 2^53)
   */
  export function toNumber(va: VirtualAddress): number {
    if (va > BigInt(Number.MAX_SAFE_INTEGER)) {
      console.warn('VirtualAddress.toNumber: precision loss for', toHex(va));
    }
    return Number(va);
  }

  /**
   * Compare two virtual addresses
   */
  export function compare(a: VirtualAddress, b: VirtualAddress): number {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }

  /**
   * Check if virtual address is valid (non-canonical addresses are invalid)
   * ARM64 uses 48-bit addresses, bits 63:48 must all be 0 or all be 1
   */
  export function isCanonical(va: VirtualAddress): boolean {
    const top16 = va >> 48n;
    return top16 === 0n || top16 === 0xFFFFn;
  }

  /**
   * Sign extend a 48-bit address to 64-bit
   */
  export function signExtend48(va: VirtualAddress): VirtualAddress {
    // Check bit 47
    if ((va & (1n << 47n)) !== 0n) {
      // Set bits 63:48 to 1
      return (va | 0xFFFF000000000000n) as VirtualAddress;
    }
    // Clear bits 63:48
    return (va & 0x0000FFFFFFFFFFFFn) as VirtualAddress;
  }

  /**
   * Create a virtual address from page table indices
   */
  export function fromIndices(
    pgd: number,
    pud: number = 0,
    pmd: number = 0,
    pte: number = 0,
    offset: number = 0
  ): VirtualAddress {
    let va = 0n;
    va |= BigInt(pgd & 0x1FF) << 39n;
    va |= BigInt(pud & 0x1FF) << 30n;
    va |= BigInt(pmd & 0x1FF) << 21n;
    va |= BigInt(pte & 0x1FF) << 12n;
    va |= BigInt(offset & 0xFFF);

    // Sign extend if in kernel space
    if (pgd >= 256) {
      va = signExtend48(va as VirtualAddress);
    }

    return va as VirtualAddress;
  }

  /**
   * Decompose virtual address into its components
   */
  export function decompose(va: VirtualAddress): {
    pgd: number;
    pud: number;
    pmd: number;
    pte: number;
    offset: number;
    isKernel: boolean;
  } {
    return {
      pgd: pgdIndex(va),
      pud: pudIndex(va),
      pmd: pmdIndex(va),
      pte: pteIndex(va),
      offset: pageOffset(va),
      isKernel: isKernel(va)
    };
  }

  /**
   * Get human-readable description of address region
   */
  export function getRegion(va: VirtualAddress): string {
    if (!isCanonical(va)) {
      return 'Non-canonical address';
    }

    if (isKernel(va)) {
      // Kernel space regions (ARM64 Linux)
      if (va >= 0xFFFF800000000000n && va < 0xFFFFC00000000000n) {
        return 'Linear mapping (direct)';
      }
      if (va >= 0xFFFFC00000000000n && va < 0xFFFFD00000000000n) {
        return 'vmalloc area';
      }
      if (va >= 0xFFFFD00000000000n && va < 0xFFFFE00000000000n) {
        return 'vmemmap';
      }
      if (va >= 0xFFFFE00000000000n) {
        return 'Kernel image/modules';
      }
      return 'Kernel space';
    }

    // User space
    const pgd = pgdIndex(va);
    if (pgd < 100) {
      return 'User space (low)';
    }
    if (pgd >= 400) {
      return 'User space (high - ASLR)';
    }
    return 'User space';
  }
}

// Export a convenience function at the top level
export function VA(value: bigint | number | string): VirtualAddress {
  return VirtualAddress.from(value);
}

// Common constants
export const VA_ZERO = VirtualAddress.ZERO;
export const VA_KERNEL_START = VirtualAddress.KERNEL_START;