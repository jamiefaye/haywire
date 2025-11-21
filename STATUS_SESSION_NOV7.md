# Windows Kernel Discovery Implementation - Session Status Nov 7, 2025

## Current Status: READY TO BUILD & TEST

All code changes are complete. Ready to rebuild and test Windows process discovery.

## What We Implemented

Created Windows kernel discovery system to scan Windows 11 guest memory for EPROCESS structures and extract process information.

### Files Created

1. **`src/windows/windows_kernel_discovery.cpp`** (336 lines)
   - Scans memory for EPROCESS structures using Windows 11 Build 26100 offsets
   - Validates process names (must end with .exe or be "System")
   - Extracts PID, process name, DirectoryTableBase (PGD)
   - Implements `IKernelDiscovery` interface

### Files Modified

2. **`src/kernel_discovery_factory.cpp`**
   - Added `#include` for Windows implementation
   - Added `GuestOS::Windows` case to factory

3. **`include/kernel_discovery_backend.h`**
   - Added `GuestOS osHint` parameter to `Initialize()`

4. **`src/kernel_discovery_backend.cpp`**
   - Passes `osHint` to factory

5. **`src/main.cpp`**
   - Added `--guest-os` command-line option (accepts "linux" or "windows")
   - Parses and stores in `guestOsHint` variable
   - Passes arch_hint to `AutoConnect()`: 1=x86_64 (Windows), 2=ARM64 (Linux)

6. **`include/memory_mapper.h`**
   - Added `arch_hint` parameter to `DiscoverMemoryMap()` and `ParseMtreeOutput()`
   - 0=auto, 1=x86_64, 2=ARM64

7. **`src/memory_mapper.cpp`**
   - Implements x86_64-specific defaults: RAM at 0x00000000, 2GB size
   - Implements ARM64-specific defaults: RAM at 0x40000000, 4GB size
   - Uses arch_hint to select correct defaults when monitor unavailable

8. **`include/memory_backend.h`**
   - Added `arch_hint` parameter to `InitializeMemoryMapping()`

9. **`src/memory_backend.cpp`**
   - Passes arch_hint through to `DiscoverMemoryMap()`

10. **`include/qemu_connection.h`**
    - Added `arch_hint` parameter to `AutoConnect()`

11. **`src/qemu_connection.cpp`**
    - Removed memory mapping initialization from constructor
    - Added it to `AutoConnect()` with arch_hint parameter
    - Now initializes memory backend with correct architecture hint

## Key Technical Details

### Windows Kernel Offsets (Windows 11 Build 26100)
```cpp
struct WindowsOffsets {
    size_t active_process_links = 0x448;    // LIST_ENTRY
    size_t unique_process_id = 0x440;       // HANDLE (PID)
    size_t image_file_name = 0x5a8;         // CHAR[15]
    size_t directory_table_base = 0x28;     // ULONG64 (in KPROCESS)
};
```

### Memory Layout Fix - THE CRITICAL BUG FIX

**Problem**: MemoryMapper was using ARM64 defaults (RAM at 0x40000000) for x86_64 Windows VMs, causing process discovery to scan the wrong memory region.

**Solution**: Architecture hint system passes through entire chain:
```
main.cpp → AutoConnect(arch_hint) → InitializeMemoryMapping(arch_hint) → DiscoverMemoryMap(arch_hint)
```

**x86_64 defaults** (Windows):
- RAM start: 0x00000000
- RAM size: 0x80000000 (2GB)

**ARM64 defaults** (Linux):
- RAM start: 0x40000000
- RAM size: 0x100000000 (4GB)

### Process Validation
- Process name must be printable ASCII
- Must end with `.exe` OR equal `"System"`
- PID must be > 0 and < 100000
- Deduplicates based on (PID, name) pairs

### Known Windows Process Names (for reference)
```cpp
"System", "smss.exe", "csrss.exe", "wininit.exe", "services.exe",
"lsass.exe", "svchost.exe", "dwm.exe", "explorer.exe", "winlogon.exe",
"RuntimeBroker", "SearchHost.exe", "StartMenuExperienceHost.exe",
"ShellExperienceHost.exe", "sihost.exe", "taskhostw.exe",
"MsMpEng.exe", "conhost.exe", "cmd.exe", "powershell.exe"
```

## Build & Test Commands

### Rebuild
```bash
cd /mnt/c/Users/jamie/haywire/build-linux
cmake --build .
```

### Kill old processes
```bash
wsl bash -c "ps aux | grep haywire | grep -v grep"  # Check for running
wsl bash -c "kill <PID>"  # Kill if needed
```

### Launch with Windows guest OS
```bash
cd /mnt/c/Users/jamie/haywire
DISPLAY=:0 ./build-linux/haywire --guest-os windows
```

### Expected Output (if successful)
```
Guest OS hint: Windows
Memory backend auto-detected and enabled!
Discovering memory regions from QEMU monitor...
MemoryMapper: Using default x86_64 memory map (monitor unavailable)
MemoryMapper: Memory regions mapping:
Region: default-ram
  GPA range: 0x0 - 0x7fffffff        # <-- Should be 0x0, NOT 0x40000000!
  Size: 2048 MB
  File offset: 0x0

[WindowsKernelDiscovery] Scanning memory for EPROCESS structures...
[WindowsKernelDiscovery] Found X processes in Yms
```

## What's Next

1. Build completes successfully
2. Launch with `--guest-os windows`
3. Verify memory layout shows RAM at 0x0 (not 0x40000000)
4. Check if EPROCESS scanning finds Windows processes
5. If no processes found, may need to adjust offsets for actual Windows 11 25H2 version

## Notes

- EPROCESS offsets are approximate and may need adjustment
- Windows 11 25H2 offsets may differ from Build 26100
- Can verify offsets using Vergilius Project: https://www.vergiliusproject.com/
- QMP connection (port 4445) is used for reading kernel structures
- Monitor connection (port 4444) is optional for memory mapping

## VM Status

- Windows 11 VM running with 8GB RAM
- Memory backend file: `/tmp/haywire-vm-mem`
- QMP port: 4445
- Monitor port: 4444

---

## Continued Session - Black Screen Fix

### Issue Discovered
After the initial implementation, testing revealed: **"It still starts black but starts working when I use the Qemu dialog to 0 copy connect"**

### Root Cause Analysis
1. Memory mapper was initialized correctly with x86_64 layout
2. But the **visualizer's memory data source was never set** in `--no-qemu` mode
3. When using the connection dialog, it triggered `SetMemoryDataSource()`
4. This revealed the missing initialization step

### Solution Implemented
Added `MappedFileMemorySource` creation and initialization in the `--no-qemu` block:

**In `src/main.cpp` (lines 162-169):**
```cpp
// CRITICAL FIX: Create memory-mapped source for visualization
auto mappedSource = std::make_shared<MappedFileMemorySource>();
if (mappedSource->OpenFile(memoryFilePath)) {
    visualizer.SetMemoryDataSource(mappedSource);
    std::cout << "Memory file mapped for visualization\n";
} else {
    std::cerr << "Warning: Could not memory-map file for visualization\n";
}
```

### Test Results - SUCCESS ✅

Launched with: `./build-linux/haywire --no-qemu --guest-os windows`

**Output:**
```
QEMU connection disabled by --no-qemu flag
Guest OS hint: Windows
Memory backend auto-detected and enabled!
Standalone mode - kernel discovery disabled
Skipping QEMU connection (--no-qemu mode)
Initializing memory mapper with default x86_64 layout
MemoryMapper: Using default x86_64 memory map (monitor unavailable)
MemoryMapper: Memory regions mapping:
================================================
Region: default-ram
  GPA range: 0x0 - 0x1ffffffff
  Size: 8192 MB
  File offset: 0x0
================================================
Memory-mapped file: /tmp/haywire-vm-mem (8192 MB)
Memory file mapped for visualization        ✅ KEY SUCCESS LINE
Memory file size: 8192 MB
Successfully mapped memory file at 0x76648fa00000
```

### Final Status: COMPLETE ✅

The `--no-qemu --guest-os windows` mode now works correctly:
- ✅ Parses command-line flags correctly
- ✅ Initializes x86_64 memory layout (8GB at GPA 0x0)
- ✅ Maps memory file for visualization
- ✅ Displays Windows memory data immediately
- ✅ No manual connection required

The black screen issue is resolved.
