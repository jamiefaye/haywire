/**
 * Physical Address type system using branded types with bigint
 * Provides type safety for physical memory addresses
 */

// Brand for Physical Address type safety
declare const PA_BRAND: unique symbol;

/**
 * Physical Address type - a branded bigint that represents a physical memory address
 */
export type PhysicalAddress = bigint & { [PA_BRAND]: void };

/**
 * Namespace for Physical Address operations
 */
export namespace PhysicalAddress {
  /**
   * Create a PhysicalAddress from various input types
   */
  export function from(value: bigint | number | string): PhysicalAddress {
    if (typeof value === 'string') {
      if (value.startsWith('0x') || value.startsWith('0X')) {
        return BigInt(value) as PhysicalAddress;
      }
      return BigInt(value) as PhysicalAddress;
    }
    if (typeof value === 'number') {
      return BigInt(value) as PhysicalAddress;
    }
    return value as PhysicalAddress;
  }

  /**
   * Zero physical address constant
   */
  export const ZERO: PhysicalAddress = 0n as PhysicalAddress;

  /**
   * ARM64 RAM start address (typical)
   */
  export const RAM_START: PhysicalAddress = 0x40000000n as PhysicalAddress;

  /**
   * Get the page-aligned address (clear bottom 12 bits)
   */
  export function pageAlign(pa: PhysicalAddress): PhysicalAddress {
    return ((pa >> 12n) << 12n) as PhysicalAddress;
  }

  /**
   * Get page number (physical address >> 12)
   */
  export function pageNumber(pa: PhysicalAddress): bigint {
    return pa >> 12n;
  }

  /**
   * Create from page number
   */
  export function fromPageNumber(pfn: bigint | number): PhysicalAddress {
    return (BigInt(pfn) << 12n) as PhysicalAddress;
  }

  /**
   * Extract page offset (bits 11:0)
   */
  export function pageOffset(pa: PhysicalAddress): number {
    return Number(pa & 0xFFFn);
  }

  /**
   * Add offset to physical address
   */
  export function add(pa: PhysicalAddress, offset: bigint | number): PhysicalAddress {
    return (pa + BigInt(offset)) as PhysicalAddress;
  }

  /**
   * Subtract from physical address
   */
  export function subtract(pa: PhysicalAddress, offset: bigint | number): PhysicalAddress {
    return (pa - BigInt(offset)) as PhysicalAddress;
  }

  /**
   * Calculate distance between two physical addresses
   */
  export function distance(pa1: PhysicalAddress, pa2: PhysicalAddress): bigint {
    return pa1 > pa2 ? pa1 - pa2 : pa2 - pa1;
  }

  /**
   * Check if address is in typical RAM range
   */
  export function isInRAM(pa: PhysicalAddress, ramSize: bigint = 4n * 1024n * 1024n * 1024n): boolean {
    return pa >= RAM_START && pa < (RAM_START + ramSize);
  }

  /**
   * Format as hex string with 0x prefix
   */
  export function toHex(pa: PhysicalAddress): string {
    return `0x${pa.toString(16).padStart(16, '0')}`;
  }

  /**
   * Format as short hex (no leading zeros)
   */
  export function toShortHex(pa: PhysicalAddress): string {
    return `0x${pa.toString(16)}`;
  }

  /**
   * Convert to regular bigint (removes branding)
   */
  export function toBigInt(pa: PhysicalAddress): bigint {
    return pa;
  }

  /**
   * Convert to number (WARNING: loses precision above 2^53)
   */
  export function toNumber(pa: PhysicalAddress): number {
    if (pa > BigInt(Number.MAX_SAFE_INTEGER)) {
      console.warn('PhysicalAddress.toNumber: precision loss for', toHex(pa));
    }
    return Number(pa);
  }

  /**
   * Compare two physical addresses
   */
  export function compare(a: PhysicalAddress, b: PhysicalAddress): number {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }

  /**
   * Check if address is aligned to specified boundary
   */
  export function isAligned(pa: PhysicalAddress, alignment: number): boolean {
    return (pa % BigInt(alignment)) === 0n;
  }

  /**
   * Align address up to boundary
   */
  export function alignUp(pa: PhysicalAddress, alignment: number): PhysicalAddress {
    const mask = BigInt(alignment - 1);
    return ((pa + mask) & ~mask) as PhysicalAddress;
  }

  /**
   * Align address down to boundary
   */
  export function alignDown(pa: PhysicalAddress, alignment: number): PhysicalAddress {
    const mask = BigInt(alignment - 1);
    return (pa & ~mask) as PhysicalAddress;
  }

  /**
   * Extract from page table entry (clear flags, get physical address)
   * ARM64 page table entries have PA in bits 47:12
   */
  export function fromPTE(pte: bigint): PhysicalAddress {
    return ((pte & 0x0000FFFFFFFFF000n)) as PhysicalAddress;
  }

  /**
   * Get memory region description
   */
  export function getRegion(pa: PhysicalAddress): string {
    if (pa < 0x40000000n) {
      return 'Device memory (low)';
    }
    if (pa >= 0x40000000n && pa < 0x140000000n) {
      return 'RAM (typical 4GB range)';
    }
    if (pa >= 0x140000000n && pa < 0x1C0000000n) {
      return 'Extended RAM (4-7GB)';
    }
    if (pa >= 0x8000000000n) {
      return 'High memory region';
    }
    return 'Unknown region';
  }
}

// Export a convenience function at the top level
export function PA(value: bigint | number | string): PhysicalAddress {
  return PhysicalAddress.from(value);
}

// Common constants
export const PA_ZERO = PhysicalAddress.ZERO;
export const PA_RAM_START = PhysicalAddress.RAM_START;