# Refactoring Session - November 6, 2025

## What We Accomplished

### Major Architectural Refactoring: OS-Agnostic Kernel Discovery

Successfully refactored the kernel discovery system to support multiple guest operating systems (Linux, Windows, etc.) with clean separation of concerns.

## Changes Made

### 1. Created Abstract Interface (`IKernelDiscovery`)

**File**: `include/ikernel_discovery.h`

- Defined common interface for all OS-specific implementations
- Common structures: `ProcessInfo`, `MemorySection`, `PTE`, `KernelInfo`
- Virtual methods: `Initialize()`, `DiscoverKernel()`, `DiscoverProcesses()`, `TranslateVA()`, etc.
- Factory pattern support: `CreateKernelDiscovery()` and `DetectGuestOS()`

### 2. Linux-Specific Implementation

**Files**:
- `src/linux/linux_kernel_discovery.cpp` (renamed from `kernel_discovery.cpp`)
- `src/linux/linux_profile_loader.h` (moved from `src/`)

**Changes**:
- Renamed `KernelDiscovery` → `LinuxKernelDiscovery`
- Inherits from `IKernelDiscovery` interface
- Added `override` keywords to all virtual methods
- Implemented Linux-specific methods: `GetOSType()` → "Linux", `GetArchitecture()` → "x86_64"/"aarch64"
- Updated profile path: `profiles` → `profiles/linux`

### 3. Directory Structure Reorganization

```
haywire/
├── include/
│   └── ikernel_discovery.h          # New: Abstract interface
├── src/
│   ├── linux/
│   │   ├── linux_kernel_discovery.cpp   # Linux implementation
│   │   └── linux_profile_loader.h       # Linux profile loader
│   ├── windows/                      # Ready for Windows impl
│   └── kernel_discovery_factory.cpp  # New: Factory functions
├── profiles/
│   ├── linux/
│   │   ├── ubuntu-6.14.0-35-x86_64.json
│   │   └── ubuntu-6.14.0-34-arm64.json
│   └── windows/                      # Ready for Windows profiles
```

### 4. Factory Pattern Implementation

**File**: `src/kernel_discovery_factory.cpp`

- `DetectGuestOS()` - Auto-detect guest OS from memory signatures (stub for now)
- `CreateKernelDiscovery()` - Factory function returning appropriate implementation
- Currently returns `LinuxKernelDiscovery`, ready for Windows support

## Benefits of This Architecture

### Clean Separation
- No `if (isWindows)` scattered throughout code
- Each OS has its own implementation file
- Changes to Linux don't affect Windows and vice versa

### Easy Testing
- Can test Linux and Windows implementations independently
- Mock implementations easy to create

### Future Extensibility
- Adding FreeBSD/macOS support is straightforward
- Just implement `IKernelDiscovery` interface
- Add case to factory function

### Profile Organization
- OS-specific profiles in separate directories
- Clear which profiles work with which OS
- Different JSON schemas for different OSes (e.g., EPROCESS vs task_struct)

## Next Steps (Not Yet Complete)

### Immediate (Required for Compilation)
1. ✅ Create interface header
2. ✅ Rename and refactor Linux implementation
3. ✅ Move profiles to OS-specific directories
4. ✅ Create factory function
5. ⏳ Update CMakeLists.txt to build new files
6. ⏳ Update all references to `KernelDiscovery` → use interface or factory

### Medium-Term (Windows Support)
1. Create `src/windows/windows_kernel_discovery.h/cpp`
2. Implement Windows EPROCESS discovery
3. Implement Windows ActiveProcessLinks walking
4. Create Windows kernel profile JSON format
5. Implement proper OS detection (scan for Windows signatures)

### Long-Term (Polish)
1. Move `KernelOffsets` to Linux-specific header
2. Create Windows-specific offset structures
3. Add comprehensive OS detection (not just stub)
4. Test with real Windows 11 VM

## Technical Debt

1. **Factory includes implementation directly**: Currently using `#include "linux/linux_kernel_discovery.cpp"` in factory - should be split into .h/.cpp
2. **OS detection is a stub**: Just returns `GuestOS::Linux` always
3. **Old kernel_discovery.cpp still exists**: Should be deleted after migration complete
4. **Profile loader not truly shared**: Each OS has separate copy, should extract common JSON parsing

## How to Use (After CMakeLists Update)

```cpp
#include "ikernel_discovery.h"

// Auto-detect OS and create appropriate backend
auto discovery = Haywire::CreateKernelDiscovery("/tmp/haywire-vm-mem");

// Or force Linux
auto linuxDiscovery = Haywire::CreateKernelDiscovery(
    "/tmp/haywire-vm-mem",
    "profiles/linux/ubuntu-6.14.0-35-x86_64.json",
    Haywire::GuestOS::Linux
);

discovery->Initialize();
discovery->DiscoverKernel();
discovery->DiscoverProcesses();

auto& processes = discovery->GetProcesses();
std::cout << "Found " << processes.size() << " processes" << std::endl;
std::cout << "Guest OS: " << discovery->GetOSType() << std::endl;
```

## Files Modified

### New Files
- `include/ikernel_discovery.h` - Interface definition
- `src/kernel_discovery_factory.cpp` - Factory implementation
- `src/linux/linux_kernel_discovery.cpp` - Linux implementation (copied from kernel_discovery.cpp)
- `src/linux/linux_profile_loader.h` - Linux profiles (copied from src/)

### Moved Files
- `profiles/ubuntu-*.json` → `profiles/linux/`

### To Be Removed (After Migration)
- `src/kernel_discovery.cpp` - Replaced by `src/linux/linux_kernel_discovery.cpp`
- `src/kernel_profile_loader.h` - Replaced by `src/linux/linux_profile_loader.h`

## Compilation Status

⚠️ **Does not yet compile** - requires CMakeLists.txt updates

The refactoring is structurally complete but needs build system updates to:
1. Add `src/linux/linux_kernel_discovery.cpp` to sources
2. Add `src/kernel_discovery_factory.cpp` to sources
3. Remove old `src/kernel_discovery.cpp` from sources
4. Update any code that directly references `KernelDiscovery` class

---

**Created**: November 6, 2025
**Status**: Architecture complete, build integration pending
