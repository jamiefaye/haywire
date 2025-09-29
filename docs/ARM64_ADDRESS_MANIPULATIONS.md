# ARM64 Address Manipulations in Haywire

## The Core Problem
ARM64 page table entries are 64 bits but not all bits are the physical address. Different bits serve different purposes, and JavaScript's number precision issues make this tricky.

## ARM64 Page Table Entry Format

```
Bits [63:48] - Attributes (UXN, PXN, software bits, etc.)
Bits [47:12] - Physical Address (36 bits, page-aligned)
Bits [11:2]  - Lower attributes (AF, SH, AP, NS, etc.)
Bits [1:0]   - Descriptor type (00=invalid, 01=block, 11=table)
```

## Critical Masks We Use

### 1. Extract Physical Address from Page Table Entry
```javascript
// WRONG - only clears low 12 bits, keeps high attribute bits
const physAddr = entry & ~0xFFF;  // Bug: gives 0x180000013ffff000

// CORRECT - masks to bits [47:12] only
const physAddr = entry & 0x0000FFFFFFFFF000;  // Correct: gives 0x13ffff000
```

### 2. Check Descriptor Type (bits [1:0])
```javascript
const descType = entry & 0x3;
// 0b00 (0) = Invalid
// 0b01 (1) = Block descriptor (huge page)
// 0b11 (3) = Table descriptor (points to next level)
```

### 3. Different Masks for Different Block Sizes
- **4KB pages**: `entry & 0x0000FFFFFFFFF000` (clear bits [11:0])
- **2MB blocks**: `entry & 0x0000FFFFFFE00000` (clear bits [20:0])
- **1GB blocks**: `entry & 0x0000FFFFC0000000` (clear bits [29:0])

## JavaScript Precision Issues

### Problem 1: Reading 64-bit Values
```javascript
// WRONG - loses precision for large values
const low = view.getUint32(offset, true);
const high = view.getUint32(offset + 4, true);
const entry = low + (high * 0x100000000);  // PRECISION LOSS!
// For 0x180000013ffff403, we get 0x180000013ffff400 (last bits become 0)

// CORRECT - use BigInt for accurate 64-bit reads
const entryBig = view.getBigUint64(offset, true);  // Returns BigInt
const entry = Number(entryBig);  // Convert to number if safe
// Or work with BigInt throughout
```

### Problem 2: High Bits in Addresses
Real example from kernel PGD:
- Entry value: `0x180000013ffff403`
- Bits [63:59]: `0x03` (attribute bits: PXN=1, etc.)
- Bits [47:12]: `0x13ffff` (actual physical address)
- Bits [11:0]: `0x403` (attributes + type bits)

Without proper masking, we interpret `0x180000013ffff000` as the physical address, which is ~96GB - way beyond our 6GB guest memory!

## Where We Apply These Manipulations

### 1. `evaluatePgdCandidate()` - Testing if a page looks like a PGD
```javascript
// Read entry accurately
const entryBig = view.getBigUint64(i * 8, true);

// Check type bits
const lowBits = Number(entryBig & 0x3n);
if (lowBits !== 0x3) continue;  // Not a valid table

// Extract physical address for next level
const pudAddrBig = entryBig & 0x0000FFFFFFFFF000n;
const pudAddr = Number(pudAddrBig);

// Check if in guest RAM range
if (pudAddr >= 0x40000000 && pudAddr < 0x1C0000000) {
    // Valid PUD table address
}
```

### 2. `translateVA()` - Walking page tables to translate virtual to physical
```javascript
// For each level (PGD → PUD → PMD → PTE):
const entry = this.memory.readU64(offset);

// Check entry type
const entryType = Number(entry) & 3;
if (entryType === 0) return null;  // Invalid
if (entryType === 1) {
    // Block descriptor - huge page
    const pageBase = Number(entry) & BLOCK_MASK;  // Mask depends on level
    return pageBase + offset_within_page;
}

// Table descriptor - continue to next level
const nextTableAddr = Number(entry) & 0x0000FFFFFFFFF000;
```

### 3. `findAllPGDs()` - Scanning memory for PGD candidates
```javascript
// Quick pre-check with proper precision
const entryBig = view.getBigUint64(i * 8, true);
const entryType = Number(entryBig & 0x3n);
if (entryType === 1 || entryType === 3) {
    // Possible page table entry
}
```

## Memory Layout Context

### Guest Physical Memory
- Start: `0x40000000` (1GB)
- Size: 6GB
- End: `0x1C0000000` (7GB)
- Kernel structures: ~`0x134000000` - `0x140000000` (around 5GB mark)

### Virtual Address Spaces
- User space: `0x0000000000000000` - `0x0000FFFFFFFFFFFF`
- Kernel space: `0xFFFF000000000000` - `0xFFFFFFFFFFFFFFFF`

### Real Kernel PGD
- Physical address: `0x136dbf000` (from TTBR1 register)
- First entry: `0x180000013ffff403`
  - Points to PUD at `0x13ffff000` (after masking)

## Common Pitfalls

1. **Forgetting high bits exist**: Entry `0x180000013ffff403` is NOT pointing to address `0x180000013ffff000`
2. **Using wrong mask**: `& ~0xFFF` only clears low 12 bits, need `& 0x0000FFFFFFFFF000` for PA
3. **JavaScript precision**: Must use `getBigUint64()` not manual 32-bit combination
4. **Wrong guest RAM size**: Was hardcoded to 8GB, actually 6GB
5. **Mixing BigInt and Number**: Be consistent, convert carefully

## Summary

The key insight is that ARM64 page table entries pack multiple pieces of information:
- High bits [63:48]: Attributes and protection
- Middle bits [47:12]: Physical address (what we want)
- Low bits [11:0]: More attributes and type

We MUST mask properly to extract just the physical address, and we MUST use JavaScript BigInt to avoid precision loss when reading 64-bit values. Without both fixes, we can't walk page tables correctly.