# x86_64 PGD Extraction Investigation Notes

**Date**: October 30, 2025
**Branch**: windows-wsl2-support

## Problem Statement

x86_64 kernel VA translation failing, causing 0% PGD extraction success rate (0/131 processes).

## Key Findings

### 1. CR3 Outside Memory File
- QMP reports CR3 at 4.2-4.3GB (e.g., `0x1128a2000`)
- Memory file is only 4GB (`/tmp/haywire-vm-mem`)
- x86_64 uses `RAM_BASE = 0x0` (no offset like ARM64's `0x40000000`)

### 2. Kernel Mappings in User PGDs
- User process PGDs contain kernel mappings (indices 256-511)
- All user processes share the same kernel entries
- Found 195 valid PGDs in memory file with 100% matching kernel entries (70 each)

### 3. Modern x86_64 Kernel Layout
- Does NOT use legacy PML4[256] for kernel text
- Uses PML4[422-486] for kernel direct mapping (65 contiguous entries)
- Additional entries: PML4[277] (VMALLOC), PML4[501,508,510,511] (fixmap, vmemmap)

### 4. x86_64 Address Layout (48-bit)
```
User space:   0x0000000000000000 - 0x00007FFFFFFFFFFF (PML4 0-255)
Kernel space: 0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF (PML4 256-511)
```

### 5. highmem=on Has No Effect
- Tested adding `highmem=on` to q35 machine config
- CR3 still at 4.08GB outside memory file
- `highmem` is ARM-specific, ignored on x86_64

## Attempted Solutions

### Solution 1: Cached Kernel Mappings (Failed - Compilation Errors)
Tried to implement cached kernel PML4 entries:
1. Scan memory file for valid PGDs
2. Extract kernel entries (indices 256-511)
3. Cache for use in `TranslateKernelVA()`
4. Use `X86_64PageWalker` for page table walking

**Issues**:
- Used non-existent `MemoryBackend::ReadPhysicalMemory()` API
- Should use `GetDirectPointer()` or `Read()` instead
- File became corrupted with sed/python fixes
- Left in broken state

### Solution 2: Use Existing X86_64PageWalker (Not Implemented)
The `X86_64PageWalker` class already exists and handles page walking.
Just needs kernel PGD to be set via `SetPageTableBase()`.

## Python Analysis Scripts Created

1. **`analyze_pgd_candidates.py`** - Analyzes PGD candidates from memory file
2. **`test_qmp_commands.py`** - Tests available QMP commands
3. **`test_qmp_read.py`** - Tests QMP memory reading

## Files Modified (To Review Before Merge)

### Scripts
- `scripts/launch_ubuntu_x86_64_linux.sh` - Added `highmem=on` (can remove, no effect)

### Source (BROKEN - DO NOT MERGE)
- `src/kernel_discovery.cpp` - Attempted kernel mapping cache (compilation errors)
  - Multiple sed/python edits corrupted the file
  - Contains syntax errors and missing functions
  - Needs to be reverted to clean state

## Recommendations for Future Work

### Short-term: Use QMP for Translation
Keep the existing `TranslateKernelVA()` approach but use `QemuConnection::ReadMemory()`:
```cpp
// Via QMP - slower but works
auto& qemu = QemuConnection::getInstance();
std::vector<uint8_t> buffer;
qemu.ReadMemory(kernelVA, 8, buffer);
```

### Long-term: Proper Implementation
1. Extract kernel PML4 mappings from any valid user PGD
2. Cache the 70 kernel entries (one-time operation)
3. Use `X86_64PageWalker` with cached entries
4. Fallback to QMP for page tables outside memory file

### Alternative: VirtualBox Support
- VirtualBox might NOT isolate kernel memory like QEMU does
- Could be easier to support than fixing QEMU x86_64 issues
- Consider separate `vbox-memexpose` project

## Useful Commands

```bash
# Check QMP CR3 value
wsl python3 test_qmp_commands.py

# Analyze PGD candidates in memory file
wsl python3 analyze_pgd_candidates.py

# Test QMP memory reading
wsl python3 test_qmp_read.py
```

## Next Steps

1. Revert broken `kernel_discovery.cpp` to clean state
2. Switch back to main branch
3. Start VirtualBox investigation (simpler path forward)
4. Create memory dumps from VBox to test Haywire
