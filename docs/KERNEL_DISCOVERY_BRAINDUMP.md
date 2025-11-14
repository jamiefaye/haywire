# Kernel Discovery Brain Dump - November 2024

## Current State Summary
We are trying to implement kernel discovery in JavaScript/TypeScript to find the kernel's swapper_pg_dir (kernel PGD) to enable proper virtual address translation. The Python implementation finds 1010 task_structs successfully, but our JavaScript implementation cannot find the real kernel PGD.

## The Core Problem
1. **Real kernel PGD location**: 0x136dbf000 (obtained via QMP from TTBR1 register)
2. **Issue**: Our PGD evaluation function fails to recognize this as valid
3. **Root cause**: JavaScript number precision issues when reading 64-bit values

## Key Technical Details

### ARM64 Page Table Format
- 4-level page tables: PGD → PUD → PMD → PTE
- Each entry is 64 bits
- Entry format:
  - Bits [47:12]: Physical address of next level table (page-aligned)
  - Bits [11:2]: Attributes/software bits
  - Bits [1:0]: Descriptor type
    - 0b00: Invalid
    - 0b01: Block descriptor (huge page)
    - 0b11: Table descriptor (points to next level)
- High bits [63:48] contain attributes like UXN, PXN, etc.

### Real PGD Entry Analysis
The first entry in the real swapper_pg_dir is: `0x180000013ffff403`
- Bits [63:59]: 0x03 (PXN=1, bit 59=1 - protection bits)
- Bits [47:12]: 0x13ffff (physical address 0x13ffff000 = ~5GB)
- Bits [11:2]: 0x100 (software/ignored bits)
- Bits [1:0]: 0b11 (valid table descriptor)

### Memory Layout
- Guest RAM: 0x40000000 to 0x1C0000000 (1GB to 7GB, total 6GB)
- Kernel code: ~0x134410000 to 0x136dbffff
- Kernel page tables: ~0x13fXXXXXX (around 5GB mark)
- Actual guest size: 6GB (not 8GB as originally hardcoded)

## JavaScript Precision Problem

### The Issue
When reading 64-bit values using 32-bit reads and combining them:
```javascript
const entryLow = view.getUint32(i * 8, true);
const entryHigh = view.getUint32(i * 8 + 4, true);
const entry = entryLow + (entryHigh * 0x100000000);  // LOSES PRECISION!
```

For the value 0x180000013ffff403:
- We read it as 0x180000013ffff400 (last 3 bits become 0)
- This makes bits [1:0] = 0b00 (invalid) instead of 0b11 (valid table)

### The Solution
Use getBigUint64 for accurate 64-bit reads:
```javascript
const entryBig = view.getBigUint64(i * 8, true);  // BigInt, accurate
const lowBits = Number(entryBig & 0x3n);  // Safe for low bits
```

## Current Code Issues Fixed

1. **GUEST_RAM_END**: Changed from 0x200000000 (8GB) to 0x1C0000000 (7GB)
2. **PTE validation**: Changed from checking exact match of 0x3 to just checking bits [1:0]
3. **64-bit reads**: Using getBigUint64 instead of combining 32-bit reads
4. **Direct physical addresses**: Page tables use direct PA, not offsets from RAM start

## Why We're Still Not Finding the Real PGD

The evaluation function (`evaluatePgdCandidate`) was failing because:
1. It couldn't read the entry correctly due to precision loss
2. The entry 0x403 in low bits was being rejected (we expected exactly 0x3)
3. We were checking the wrong memory range boundaries

## What's Working
- Process discovery: Finding 122 processes successfully
- Memory reading: PagedMemory handles 6GB file correctly
- QMP integration: Successfully getting real kernel PGD address

## What's Not Working
- PGD validation: Even with correct address, can't validate it
- Page table walking: Can't follow chains without valid kernel PGD
- Virtual address translation: Need kernel PGD for kernel VAs

## Next Steps
1. Test the precision fixes - the real PGD should now be recognized
2. Verify page table walking works with corrected reads
3. Implement proper virtual-to-physical translation
4. Extract process memory maps using validated kernel PGD

## Important Constants
```typescript
KernelConstants = {
    GUEST_RAM_START: 0x40000000,    // 1GB
    GUEST_RAM_END: 0x1C0000000,     // 7GB (1GB + 6GB)
    PAGE_SIZE: 4096,

    // task_struct offsets
    PID_OFFSET: 0x5A8,
    COMM_OFFSET: 0x6A0,
    MM_OFFSET: 0x570,
    TASKS_LIST_OFFSET: 0x380,

    // mm_struct offsets
    PGD_OFFSET: 0x48,
}
```

## Debugging Notes
- Use QMP to get ground truth (real swapper_pg_dir via query-kernel-info)
- The kernel PGD has very few entries (usually just index 0 for kernel mappings)
- User processes have more distributed PGD entries
- False positive PGDs often have random data that happens to look valid

## Historical Issues We've Hit
1. Precision loss with JavaScript numbers > 2^53
2. Endianness confusion
3. Mixing offsets vs direct physical addresses
4. Hardcoded memory limits not matching actual guest size
5. Too strict validation (expecting exact bit patterns)