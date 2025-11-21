# Windows Kernel Discovery Session - November 7, 2025 (Part 3)

## What We Accomplished

### ✅ Integrated Windows EPROCESS Discovery
- Created `windows_kernel_discovery.cpp` with profile-based EPROCESS scanning
- Implemented VA→PA translation for Windows x64 page tables (4-level paging)
- Properly handles huge pages (1GB, 2MB)

### ✅ Two Approaches Attempted

**Approach 1: Blind Physical Memory Scan**
- Scanned all 8GB RAM looking for EPROCESS-like structures
- With loose validation: Found 2515 false positives (garbage data)
- With strict validation (.exe requirement): Found 8 processes including:
  - msedge.exe (DTB=0x1c5000) ✓
  - mspaint.exe (DTB=0xa134000) ✓
  - cmd.exe (DTB=0xc1200000) ✓
  - Some truncated names: "ost.exe", "ync.exe" (alignment issues)

**Approach 2: QMP-Based List Walking (Current)**
- Find System process (PID 4) to get kernel CR3
- Walk ActiveProcessLinks using VA→PA translation
- **Current Issue**: First VA translation fails

## Current Problem

```
Kernel CR3: 0x8948106889480858  ← WRONG! Should be ~8GB max
System EPROCESS VA: 0x747441000076fbb8
Failed to translate VA 0x747441000076fbb8
```

**Root Cause**: The DTB value we read from System process at offset 40 is garbage (0x8948106889480858). This means:
1. We found the wrong System process candidate (PA 0x2966be51), OR
2. The DirectoryTableBase offset (40) is wrong for this Windows build, OR
3. We're not accounting for pool headers before EPROCESS

## Key Insights from Session

1. **EPROCESS pool headers**: EPROCESS structures in physical memory have variable-size headers (POOL_HEADER + OBJECT_HEADER) before them
2. **Alignment varies**: Some processes at 16-byte boundaries (perfect names), others misaligned (truncated names)
3. **Unicode is NOT the issue**: ImageFileName field is ASCII CHAR[15], not Unicode
4. **Process count**: Windows should have 50-100+ processes, but we're only finding a few

## Next Steps for Monday

### Option A: Fix the QMP Approach (Proper Way)
1. Verify DirectoryTableBase offset is correct (currently using offset 40 from profile)
2. Add pool header parsing to find true EPROCESS start
3. Validate System process DTB looks reasonable (should be < 8GB)

### Option B: Improve Blind Scan (Pragmatic Way)
1. Accept the 8 processes we found (3 confirmed good: msedge, mspaint, cmd)
2. Test if their page tables work for VA mode
3. Iterate on improving detection later

### Option C: Use QMP for Everything
1. Get PsActiveProcessHead kernel symbol VA from QMP
2. Get kernel CR3 from QEMU CPU state (not from System process)
3. Walk list translating each VA with known-good kernel CR3

## Files Modified
- `src/windows/windows_kernel_discovery.cpp` - Main implementation
- `src/kernel_discovery_factory.cpp` - Added Windows backend creation
- `profiles/windows/windows-11-26100-x86_64.json` - Structure offsets

## Recommendation
Start with **Option C** on Monday - get kernel CR3 from QEMU CPU state directly, which we know is correct, rather than trying to read it from a potentially misaligned EPROCESS structure.
