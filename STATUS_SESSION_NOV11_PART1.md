# Windows Kernel Discovery Session - November 11, 2025 (Part 1)

## What We Accomplished

### ✅ Implemented Option C: Get CR3 from CPU Registers
- Added `QueryCR3()` method to `QemuConnection` class
- Uses QMP `human-monitor-command` with "info registers"
- Parses CR3 for x86_64 and TTBR1_EL1 for ARM64
- **Result**: Successfully getting CR3 = `0x1ae000` (valid value within 8GB RAM)

###  ✅ Implemented Option 2: Try All System Process Candidates
- Modified `FindSystemProcess()` → `FindAllSystemProcesses()` to return all candidates
- Updated `DiscoverProcesses()` to try each candidate until one works
- Added validation check for kernel VAs (must start with `0xFFFF...`)

## Current Status

### System Process Candidates Found

**Candidate #1** at PA `0x65c357c1`:
- Flink: `0x11ffffff`
- Calculated VA: `0x11fffbb7` ❌ NOT a kernel address
- Status: **REJECTED** (invalid VA format)

**Candidate #2** at PA `0xa2696dd0`:
- Flink: `0xffff820fc785b258`
- Calculated VA: `0xffff820fc785ae10` ✓ **Valid kernel VA!**
- Status: **PROMISING** but VA→PA translation failing

**Candidate #3** at PA `0x17436bf98`:
- Flink: `0x144000c00000000`
- Calculated VA: `0x144000bfffffbb8` ❌ NOT a kernel address
- Status: **REJECTED** (invalid VA format)

### Problem with Candidate #2

Even though we have:
- Valid kernel CR3: `0x1ae000`
- Valid kernel VA: `0xffff820fc785ae10`

The VA→PA translation is failing:
```
[TranslateVA] VA=0xffff820fc785ae10 PGD=0x1ae000 PML4_idx=104
MemoryBackend::Read failed: GPA=0xa00000101c121f8 fileOffset=0xa000000c1c121f8 mappedSize=0x200000000
```

**Analysis**:
- PML4 index 104 (0x68) is correct for kernel space
- PML4 entry address: `0x1ae000 + (104 * 8) = 0x1ae340`
- But we're trying to read from `0xa00000101c121f8` (10GB+) ← **Garbage!**

**Possible Causes**:
1. The PML4 entry at index 104 contains garbage/invalid data
2. The CR3 (`0x1ae000`) might be a **user process CR3**, not the kernel's
3. Windows might use a different paging structure than standard x64
4. The PML4 entry might not be present (bit 0 not set)

## Next Steps

### Option A: Verify CR3 is Kernel CR3
- Check if `0x1ae000` is actually the System process's CR3 or a user process's
- Try reading the PML4 entry at `0x1ae340` directly and see what it contains
- Verify bit 0 (Present) is set

### Option B: Find PsActiveProcessHead Symbol
- Instead of starting from System EPROCESS, find the global symbol
- Pattern scan for `PsActiveProcessHead` in kernel memory
- This would bypass the whole "find System process" problem

### Option C: Use QEMU's VA→PA Translation
- Add QMP command to let QEMU do the translation for us
- QEMU knows the current page tables and can translate correctly
- Would bypass our buggy TranslateVA implementation

## Files Modified
- `include/qemu_connection.h` - Added `QueryCR3()` declaration
- `src/qemu_connection.cpp` - Implemented `QueryCR3()` with register parsing
- `src/windows/windows_kernel_discovery.cpp`:
  - Changed `FindSystemProcess()` → `FindAllSystemProcesses()`
  - Modified `DiscoverProcesses()` to try all candidates
  - Added VA validation check (must start with `0xFFFF...`)
  - Added debug output to `TranslateVA()`

## Key Insights

1. **CR3 from CPU works!** - No more garbage values like `0x8948106889480858`
2. **Candidate #2 has valid kernel VA** - The structure is correctly aligned at `0xa2696dd0`
3. **Problem is in page table walking** - Not in finding the System process
4. **PML4 index 104 is reasonable** - It's in the kernel half (256-511 range)

## Recommendation

Try **Option C** first - use QEMU's existing VA→PA translation via QMP. This would:
- Bypass our potentially buggy TranslateVA implementation
- Let QEMU handle all the page table complexity
- Work regardless of paging structure differences

We already have `TranslateVA2PA()` in QemuConnection that does this!
