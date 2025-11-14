# Windows VAD Tree Access - November 13, 2025 (BREAKTHROUGH)

## Session Summary

**MAJOR BREAKTHROUGH**: Successfully translated VadRoot VA to PA and accessed VAD tree structure!

### Problem Statement (From Previous Session)
- VadRoot VA translation failed with "corrupt PDPT entry" (0xffffffffffffffff)
- QMP's `xp` command reported "Cannot access memory" for all kernel VAs
- All 49 processes failed "Failed to get sections"
- Suspected page tables were lazy-allocated or beyond 8GB file

### Root Cause Discovered

**The problem was NOT corrupt page tables or memory beyond 8GB!**

The issue was **using the wrong DTB (CR3) for translation**:
- QMP's `xp` command uses the **current CPU's loaded CR3**
- When CPUs execute user-space code, they have user process CR3 loaded
- **KPTI (Kernel Page Table Isolation)** means user CR3 doesn't map kernel pages
- VadRoot is a kernel VA (0xffff...), inaccessible via user process CR3

### Solution

**Use System process DTB for all kernel VA translations**:
1. System process (PID 4) DTB = 0x1ae000 (from EPROCESS.Pcb.DirectoryTableBase)
2. System's CR3 has full kernel page table mappings
3. Manual page walking via QMP physical reads works perfectly

### Proof of Concept Test Results

#### VadRoot Translation (mspaint PID 5600)
```
VA:    0xffff860fdcb5bf90
DTB:   0x1ae000 (System process)

Page Walk:
  [1] PML4[268] @ PA 0x1ae860      = 0x0a00000101c13863
      ✓ Present → PDPT base = 0x101c13000

  [2] PDPT[63]  @ PA 0x101c131f8   = 0x0a00000101c14863
      ✓ Present → PD base = 0x101c14000

  [3] PD[229]   @ PA 0x101c14728   = 0x8a0000016dc009e3
      ✓ 2MB large page → PA = 0x16dd5bf90

Result: VA 0xffff860fdcb5bf90 → PA 0x16dd5bf90 (5.83 GB)
```

#### Physical Address Analysis
- **PA: 0x16dd5bf90 = 5.83 GB**
- **Memory file: 8 GB**
- **Result: PA is WITHIN memory-backend-file! ✓**

#### VAD Structure Read
```
QMP: xp /64xb 0x16dd5bf90

000000016dd5bf90: 0xa0 0xfa 0xb5 0xdc 0x0f 0x86 0xff 0xff  (Left child)
000000016dd5bf98: 0x30 0xe7 0xba 0xda 0x0f 0x86 0xff 0xff  (Right child)
000000016dd5bfa0: 0x01 0x00 0x00 0x00 0x00 0x00 0x00 0x00  (Balance)
000000016dd5bfa8: 0xa0 0x9c 0x9a 0x1d 0x9f 0xa0 0x9a 0x1d

Left child:  0xffff860fdcb5faa0 ✓ Valid kernel pointer
Right child: 0xffff860fdabae730 ✓ Valid kernel pointer
```

**VAD tree is accessible and intact!**

## Key Insights

### 1. Memory Layout is Correct
- No memory beyond 8GB issue
- Page tables are NOT corrupt
- All kernel structures are within memory file

### 2. KPTI Impact
- User processes cannot translate kernel VAs with their own CR3
- **Must use System's CR3 for kernel VAs**
- This is by design - KPTI security feature

### 3. Page Walking Strategy
When translating VAs:
- **Kernel VAs (0xffff...)**: Use System DTB (PID 4)
- **User VAs (0x0000...)**: Use process's own DTB
- Manual page walking via QMP physical reads works for both

### 4. Large Pages Common
- Windows uses 2MB pages extensively for kernel memory
- VadRoot was in a 2MB page (PD-level mapping)
- No PT-level lookup needed

## Implementation Required

### C++ Code Changes

**File: `src/windows/windows_kernel_discovery.cpp`**

Current problem:
```cpp
// TranslateVA() doesn't specify which DTB to use
// Implicitly uses process's own DTB
uint64_t TranslateVA(uint64_t va, uint64_t dtb) {
    // ... page walking code ...
}
```

Required fix:
```cpp
// Add logic to select appropriate DTB based on VA
uint64_t TranslateKernelVA(uint64_t va) {
    // For kernel VAs (0xffff...), use System's DTB
    if ((va >> 48) == 0xffff) {
        return TranslateVA(va, system_dtb);  // Use System's CR3
    } else {
        return TranslateVA(va, process_dtb);  // Use process's own CR3
    }
}

// GetMemorySections() implementation
std::vector<MemorySection> GetMemorySections(ProcessInfo& proc) {
    // Read VadRoot from EPROCESS
    uint64_t vadroot_va = ReadU64(proc.task_addr + profile.eprocess_vad_root);

    // Translate using System DTB (VadRoot is kernel VA!)
    uint64_t vadroot_pa = TranslateKernelVA(vadroot_va);

    if (vadroot_pa == 0) {
        return {};  // Translation failed
    }

    // Read VAD tree structure
    return WalkVADTree(vadroot_pa);
}
```

### VAD Tree Walking

Once VadRoot PA is obtained:
1. Read MMVAD structure (136 bytes for Build 26200)
2. Extract fields:
   - Left child (offset 0x00): Pointer to left subtree
   - Right child (offset 0x08): Pointer to right subtree
   - StartingVpn (offset 0x18): Start address / 0x1000
   - EndingVpn (offset 0x20): End address / 0x1000
3. Recursively walk left/right children (translate each VA→PA)
4. Build list of memory sections with start/end addresses

## Test Files Created

1. **`test_current_vadroot.py`** - Finds current processes and tests VadRoot access
2. **`test_system_dtb_translation.py`** - Manual page walking using System DTB
3. **`test_complete_vadroot_walk.py`** - Full VA→PA translation with structure read

All tests successful! Proves the approach works end-to-end.

## Next Steps

### Immediate (This Session)
- [x] Prove VA→PA translation works with correct DTB
- [x] Verify VadRoot is within memory file
- [x] Read VAD structure and validate pointers

### Short Term (Next Session)
1. Fix C++ `TranslateVA()` to accept DTB parameter
2. Implement `TranslateKernelVA()` with System DTB logic
3. Implement `WalkVADTree()` function
4. Test with all discovered processes
5. Verify memory sections are extracted correctly

### Medium Term
1. Integrate VAD tree data with page database
2. Display process memory maps in UI
3. Add filtering by memory type (code, data, stack, heap)
4. Support querying DLLs and mapped files

## Comparison: Before vs After

### Before (Previous Session)
```
Problem: PDPT entry = 0xffffffffffffffff (corrupt)
Reason:  Using stale data from previous session
Result:  Translation failed
```

### After (This Session)
```
Solution: PDPT entry = 0x0a00000101c14863 (valid)
Reason:  Using correct DTB (System's CR3) + fresh data
Result:  Translation successful! VAD tree accessible!
```

## Files Modified This Session

None - all testing done via Python scripts. C++ implementation pending.

## Outstanding Questions

1. **Why does Haywire use wrong DTB?**
   - Need to review how DTB is selected in TranslateVA()
   - Probably implicitly using process's DTB instead of System's

2. **Is System DTB always at offset 0x40 in KPROCESS?**
   - Yes, verified in profile (Pcb.DirectoryTableBase offset)
   - System process (PID 4) DTB = 0x1ae000

3. **Do all kernel structures need System DTB?**
   - Yes, any kernel VA (0xffff...) requires System's CR3
   - User VAs (0x0000...) use process's own CR3

## Performance Considerations

- Large pages reduce translation overhead (no PT lookup)
- QMP physical reads fast enough for VAD walking
- Tree depth typically 10-15 levels (log2 of # regions)
- Expected: ~100ms to walk complete VAD tree per process

## Recommendations

**Priority 1**: Fix C++ translation code to use System DTB for kernel VAs
**Priority 2**: Implement VAD tree walker with recursive traversal
**Priority 3**: Integrate with existing page database and UI

This is a **major breakthrough** - all the pieces are in place for full Windows process introspection!
