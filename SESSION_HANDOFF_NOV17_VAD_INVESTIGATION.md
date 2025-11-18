# Session Handoff - November 17, 2025 - VAD Tree Investigation

## Problem Statement

Windows process selection in Haywire doesn't work - selecting mspaint.exe (or any process) shows no memory sections. VAD tree walking is failing.

## What We Discovered Today

### 1. VadRoot Offset is CORRECT ✓
- Profile uses offset **1368** (loaded from `profiles/windows/windows-11-26100-x86_64.json`)
- Verified by reading EPROCESS directly from mmap file
- mspaint.exe EPROCESS at PA 0x8afd0080 + 1368 = **0xffffb18ac77b02c0**
- This IS a valid kernel VA (top 16 bits = 0xffff)

### 2. VadRoot VAs Cannot Be Translated ✗
**Process DTBs**: Most have PML4E=0 (not mapped) for kernel VA range
**System DTB (0x1ae000)**: Has PML4E=0xa0000007f012863 (valid) but PDPTE=0 (not present)

Example for mspaint PID 6728:
```
VadRoot VA: 0xffffb18ac77b02c0
Process DTB (0x16fbed000): PML4E=0 (not mapped)
System DTB (0x1ae000): PML4E=0xa0000007f012863 → PDPT at PA 0x7f012000
  PDPT at 0x7f012000: ALL ZEROS (verified via QMP dump)
```

### 3. Direct QMP Reads Also Fail ✗
- Tried reading VadRoot VA 0xffffb18ac77b02c0 directly via QMP: "Cannot access memory"
- EPROCESS itself at PA 0x8afd0080 also returns "Cannot access memory" via QMP
  - BUT successfully readable via mmap file!

### 4. The Nov 14 Mystery 🤔

The Nov 14 handoff claimed VAD tree walking was working:
```
3. **VAD Tree Walking**
   - Full recursive traversal (573 regions for mspaint)
   - QMP-based reading (required for kernel VAs)
```

**HOW?** If VadRoot VAs can't be translated and can't be read directly, how did it work before?

## Investigation Status

### Verified Working:
- ✅ Process discovery (92 processes found)
- ✅ EPROCESS structure reading (PIDs, names, DTBs extracted correctly)
- ✅ VadRoot offset (1368 confirmed correct)
- ✅ VadRoot VA extraction (valid kernel VAs)
- ✅ Memory mapping (ReadMemory uses mmap correctly after Nov 17 fix)

### Broken:
- ❌ VadRoot VA → PA translation (all DTBs fail)
- ❌ VAD tree walking (no sections found for any process)
- ❌ QMP direct VA reads (returns "Cannot access memory")

## Next Steps for Tomorrow

### Priority 1: Check WalkVADTree Implementation
Look at the actual `WalkVADTree` function in `windows_kernel_discovery.cpp`:
- What PA is it passing to read VAD nodes?
- Is it using a different translation method?
- Does it have a fallback mechanism?

### Priority 2: Compare with Nov 14 Code
The Nov 14 handoff says VAD tree walking was working. Check:
- What changed between then and now?
- Was the code using `ReadMemoryViaQMP` directly instead of translating?
- Did QMP VA reads work before?

### Priority 3: Test Alternative Approaches
1. **Direct physical scan**: Maybe VAD nodes are at predictable physical locations?
2. **Different VA interpretation**: Maybe VadRoot is an offset, not an absolute VA?
3. **Kernel direct mapping**: Windows might have a direct-map region like Linux

### Priority 4: Verify Windows Build
The profile is for Build 26200, but VM might be different build after updates.
Check actual build: VNC → Settings → System → About

## Files Modified Today

- `src/windows/windows_kernel_discovery.cpp`:
  - Added debug output showing VadRoot offset being used
  - Added dual-DTB attempt (try process DTB first, fall back to System DTB)
  - Lines 312-354: VadRoot translation logic

## Key Technical Details

### EPROCESS Structure (mspaint PID 6728):
- **Physical Address**: 0x8afd0080
- **UniqueProcessId** (offset 464): 6728
- **ImageFileName** (offset 824): "mspaint.exe"
- **VadRoot** (offset 1368): 0xffffb18ac77b02c0
- **DTB** (offset 40): 0x16fbed000

### System Process:
- **Physical Address**: 0x8ecae040
- **DTB**: 0x1ae000
- PML4 index 163 → PML4E = 0xa0000007f012863
- PDPT at PA 0x7f012000 → ALL ZEROS

### Memory Regions:
```
ram-below-4g:  0x00000000 - 0x7FFFFFFF (2GB) → file[0x0 - 0x7FFFFFFF]
ram-above-4g:  0x100000000 - 0x27FFFFFFF (6GB) → file[0x80000000 - 0x1FFFFFFFF]
```

## Questions for Tomorrow

1. How was VAD tree walking working in Nov 14? Code comparison needed.
2. Are VadRoot pointers supposed to be physical addresses in disguise?
3. Does QMP have a special VA read command we're not using?
4. Is there a Windows direct-map region we should use instead of page tables?

## Current State

- **VM**: Running, Windows 11, mspaint.exe open (PID 6728, 6716)
- **Haywire**: Built successfully, process discovery works
- **Git**: Changes not committed (debug output additions)

---

**Great detective work today!** We proved the VadRoot offset is correct and isolated the exact failure point (VA translation). Tomorrow we'll figure out how Nov 14 session made it work.
