# Windows Guest Memory Layout - November 13, 2025 (Machine Type Testing)

## Session Summary

Tested changing QEMU machine type from q35 to pc to see if it would keep all memory allocations within the 8GB memory-backend-file.

### Key Finding: **Machine Type Doesn't Solve The Problem**

Both machine types still have extended memory regions beyond the 8GB file:

## Memory Layout Comparison

### q35 Machine (Original)
```
Physical Memory Layout:
  0-3GB:    ram-below-4g (3GB)
  4-10GB:   ram-above-4g (6GB)
  Total: 3GB + 6GB = 9GB advertised

Memory File: 8GB (0-8GB file offsets)
Extended Region: 8-10GB (accessible via QMP, not in file)
```

### pc Machine (i440FX)
```
Physical Memory Layout:
  0-3GB:    ram-below-4g (3GB)
  4-9GB:    ram-above-4g (5GB)
  Total: 3GB + 5GB = 8GB advertised

Memory File: 8GB (0-8GB file offsets)
Extended Region: 8-9GB (accessible via QMP, not in file)
```

**Conclusion**: Both machine types advertise more physical address space than fits in the memory-backend-file.

## Why This Happens

On x86_64 with 8GB RAM:
- Physical addresses 0-3GB: Low RAM (minus PCI hole at 0xC0000000-0xFFFFFFFF)
- Physical addresses 4GB+: High RAM (remaining 5-6GB)
- **Problem**: Even though we only asked for 8GB RAM, QEMU maps it to physical addresses that span 9-10GB

## Test Results

### QMP Memory Access (with pc machine)
```
✓ 0x1000000 (16 MB):       Readable
✓ 0x80000000 (2 GB):       Readable
✓ 0x100000000 (4 GB):      Readable
✓ 0x1f0000000 (7.75 GB):   Readable
✓ 0x220000000 (8.5 GB):    Readable (via QMP, NOT in file)
✗ 0x26aeece40 (9.67 GB):   "Cannot access memory"
✗ 0x280000000 (10 GB):     "Cannot access memory"
```

### The Core Issue Remains

Windows allocates page tables at physical addresses **beyond the memory-backend-file**:
- Memory file covers: 0 - 8GB
- Page tables located at: ~9.67 GB (confirmed in previous session)
- These page tables are accessible via QMP
- These page tables are NOT accessible via memory file

## What We Tried

1. **Changed machine type from q35 to pc**
   - Expected: Simpler memory layout, all within 8GB
   - Result: Still has extended region (8-9GB instead of 8-10GB)
   - Verdict: Doesn't solve the problem

2. **Attempted to verify PT addresses with translation test**
   - Created test scripts to trace VA→PA translation
   - Haywire crashed during testing
   - Unable to complete verification

## Haywire Stability Issues Discovered

**Problem**: Haywire crashes consistently during memory scanning
- Crashes around 2048MB scanned (before completing 8GB scan)
- Happens with both q35 and pc machine types
- VM remains stable, only Haywire dies

**Possible Causes**:
1. Bug in memory scanning code
2. Reading invalid memory addresses causes crash
3. Profile offset errors triggering segfault
4. QMP connection issues

## VM Status

✅ **VM Running Stable** with q35 machine type (reverted from pc)
- QEMU process: PID 98068
- Machine: q35
- RAM: 8GB
- QMP: localhost:4445 (working)
- Monitor: localhost:4444
- Memory file: /tmp/haywire-vm-mem (8GB)

## Files Modified This Session

1. **scripts/launch_windows11.sh**
   - Changed: `-machine pc` (tested)
   - Reverted to: `-machine q35` (original)
   - Final state: Using q35 (stable configuration)

2. **Test scripts created:**
   - `test_pc_machine_memory.py` - Memory layout tester
   - `test_vadroot_translation.py` - VA→PA translation debugger (incomplete)
   - `test_kernel_va_translation.py` - Kernel VA translation test (blocked by user)

## Remaining Questions

1. **Why does Windows allocate page tables beyond advertised RAM?**
   - Is this normal x86_64 behavior?
   - Is QEMU inadvertently advertising extra memory regions?
   - Could be related to UEFI firmware allocations?

2. **Why is Haywire crashing?**
   - Needs investigation with debugger
   - May be related to profile offsets
   - Could be reading unmapped memory

3. **How to access memory beyond 8GB?**
   - Option A: Make QMP fallback work in C++ code
   - Option B: Increase memory-backend-file size to 10GB
   - Option C: Reduce RAM to 4GB or less (everything below 4GB)

## Next Steps

### Immediate Priority: Fix Haywire Crashes
1. Run Haywire under gdb to catch crash
2. Check if it's related to specific memory addresses
3. Verify profile offsets aren't causing invalid reads

### Medium Term: Solve 8GB+ Memory Access
1. Verify QMP fallback code paths in `windows_kernel_discovery.cpp`
2. Add debug output to see why QMP fallback isn't triggered
3. Test if increasing file size to 10GB helps

### Long Term: Memory Layout Investigation
1. Understand why Windows uses physical addresses beyond RAM
2. Check if UEFI firmware creates reserved regions
3. Investigate if reducing RAM to 4GB eliminates extended region

## Key Insights

1. **Machine type is not the solution** - Both q35 and pc have extended regions
2. **QEMU always creates extended region** - For 8GB RAM on x86_64
3. **QMP can access extended memory** - Proven in tests
4. **C++ QMP fallback exists** - Just not being triggered
5. **Haywire has stability issues** - Needs debugging before further testing

## Recommendations

**For this session**: Stop here, document findings, investigate Haywire crashes

**For next session**:
1. Debug Haywire crash (run under gdb)
2. Once stable, test VA→PA translation properly
3. Determine actual PT addresses via validated translation
4. Confirm if PT really is beyond 8GB or if our calculation is wrong

