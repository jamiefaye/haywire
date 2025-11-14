# VirtualBox Build on Windows - Status Report

## Date: November 4, 2025

## Objective

Build VirtualBox from source on Windows to implement a shared memory patch that exposes guest physical memory (including kernel structures at 4GB+) as a memory-mapped file for Haywire introspection.

## What We Accomplished

### ✅ Prerequisites Installed

1. **Visual Studio 2022 Community** - MSVC compiler (VCC143 toolset)
2. **Windows DDK v7.1** - Legacy driver development kit at `C:\WinDDK\7600.16385.1`
3. **Windows 10 SDK/WDK** - Modern SDK at `C:\Program Files (x86)\Windows Kits\10`
4. **kBuild** - VirtualBox build system via Git at `C:\Users\jamie\vbox-src\kBuild\kBuild`
5. **yasm 1.3.0** - Assembler at `C:\Users\jamie\vbox-src\tools\win.amd64\bin\yasm.exe`

### ✅ VirtualBox Source Configured

**Location:** `C:\Users\jamie\vbox-src`

**Configuration Success:**
```
cd C:\Users\jamie\vbox-src
configure_vbox.cmd
```

Generated files:
- `env.bat` - Build environment setup
- `AutoConfig.kmk` - Build configuration
- `LocalConfig.kmk` - Local overrides (hardening disabled, GUI disabled)

### ✅ Source Modifications for VS 2022

Modified `configure.vbs` to support Visual Studio 2022:

**Lines 843-848** - Added VCC143 compiler support:
```vbscript
elseif InStr(1, strVer, "19.3") = 1 then
   m_strVersion = "VCC143"
   LogPrint "Visual Studio 2022 detected - using VCC143 toolset"
elseif InStr(1, strVer, "19.4") = 1 then
   m_strVersion = "VCC143"
   LogPrint "Visual Studio 2022 detected - using VCC143 toolset"
```

**Lines 1791-1798** - Added VCC143 Qt MSVC infixes:
```vbscript
if     g_strVCCVersion = "VCC143" then
   arrVccInfixes = Array("msvc2022_64", "msvc2019_64", "msvc2017_64")
elseif g_strVCCVersion = "VCC142" or g_strVCCVersion = "" then
   arrVccInfixes = Array("msvc2019_64", "msvc2017_64", "msvc2015_64")
```

### ✅ Build Configuration

**LocalConfig.kmk** created with:
```make
VBOX_WITH_HARDENING :=      # Disable code signing requirement
VBOX_WITH_QTGUI :=          # Disable Qt GUI
VBOX_WITH_VBOXSDL :=        # Disable SDL frontend
```

This avoids need for $400+/year Windows code signing certificates.

## Where We Got Stuck

### ❌ Build Error: "Invalid number 'ev'"

**Exact error output:**
```
C:/Users/jamie/vbox-src/src/VBox/Main/webservice/Makefile.kmk:115: VBOX_PATH_GSOAP not found...
C:/Users/jamie/vbox-src/kBuild/kBuild/units/qt6.kmk:235: kBuild: Couldn't find the Qt6 headers and libaries...
kmk: *** Invalid number "ev".  Stop.
```

**Error Context:**
- Happens after qt6.kmk is loaded
- kmk tries to parse something as a number and gets "ev" instead
- Warnings about GSOAP and Qt6 are normal (we disabled those)
- The "Invalid number" is the fatal error

### Root Cause Analysis

**Suspected issues:**

1. **Windows environment variable parsing** - kmk may be trying to parse `PROCESSOR_REVISION=ba02` (hex value) or similar Windows env vars as decimal numbers

2. **Qt6 unit file** - Even though Qt6 is disabled, `qt6.kmk` is still loaded and may reference variables that don't exist or have unexpected values

3. **kBuild on Windows** - VirtualBox is primarily developed on Linux; kBuild's Windows support may have assumptions that break with certain environment configurations

### What We Tried

1. ✅ **Visual Studio environment initialization** - Confirmed vcvarsall.bat works in test script
2. ✅ **Minimal PATH** - Reset PATH to minimal before vcvarsall.bat
3. ✅ **Unset problematic vars** - Tried unsetting PROCESSOR_LEVEL, PROCESSOR_REVISION
4. ✅ **Developer Command Prompt** - Tried running from pre-initialized VS environment
5. ✅ **Disabled Qt in LocalConfig.kmk** - Set VBOX_WITH_QTGUI :=
6. ❌ **Debug output** - Attempted kmk --debug=v but log wasn't created
7. ❌ **Multiple build scripts** - All hit same error

## Build Scripts Created

### `C:\Users\jamie\vbox-src\configure_vbox.cmd`
Automated configuration with correct paths for DDK, SDK, yasm, and disabled SDL.

### `C:\Users\jamie\vbox-src\build.cmd`
Simplified build script that:
- Sets minimal PATH
- Calls vcvarsall.bat
- Sets kBuild environment
- Runs kmk

### `C:\Users\jamie\vbox-src\build_clean.cmd`
Variant that unsets problematic environment variables before building.

### `C:\Users\jamie\vbox-src\test_env.cmd`
Diagnostic script - **This worked!** Confirmed:
- ✅ vcvarsall.bat initializes correctly
- ✅ Visual Studio tools found
- ✅ kmk runs and shows version
- ❌ Full build fails with "Invalid number" error

## Files and Locations

### Source
```
C:\Users\jamie\vbox-src\
├── configure.vbs           (modified for VS 2022)
├── configure_vbox.cmd      (configuration script)
├── LocalConfig.kmk         (build overrides - hardening off)
├── AutoConfig.kmk          (generated config)
├── env.bat                 (generated environment)
├── build.cmd               (build script)
├── build_clean.cmd         (build with clean env)
└── kBuild\                 (build system)
    └── kBuild\
        └── bin\win.amd64\
            └── kmk.exe     (make tool)
```

### Prerequisites
```
C:\WinDDK\7600.16385.1\                                    (DDK 7.1)
C:\Program Files (x86)\Windows Kits\10\                    (SDK 10)
C:\Program Files\Microsoft Visual Studio\2022\Community\  (VS 2022)
```

## Documentation Created

1. **`STATUS_SESSION_NOV3.md`** - Previous session context and plan
2. **`STATUS_SESSION_NOV4.md`** - Today's progress with configure success
3. **`BUILD_INSTRUCTIONS.md`** - Complete build instructions
4. **`INSTALL_DDK_MINIMAL.md`** - DDK installation guide
5. **`DOWNLOAD_OLD_DDK.md`** - Where to get DDK 7.1
6. **`docs/vbox_memory_access_final_plan.md`** - Implementation plan for memory patch

## The Plan (If We Return to This)

### Immediate Next Steps

1. **Investigate qt6.kmk parsing** - Add debug output to see what variable causes "ev" error
2. **Try completely removing Qt6 unit** - Comment out qt6.kmk include in main makefiles
3. **Build driver only** - Try `kmk -C src/VBox/HostDrivers/Support` to skip Qt entirely
4. **Install Qt6** - Download Qt 6 for MSVC 2022 (3GB) - might fix the qt6.kmk parsing

### Workarounds to Try

1. **Build on Linux** - VirtualBox is developed on Linux, build process is cleaner there
   - Could use WSL2 Ubuntu
   - Would build Linux binaries, not Windows driver we need

2. **Cross-compile from Linux** - Build Windows binaries FROM Linux
   - More complex but follows Oracle's actual build process

3. **Use pre-built VirtualBox** - Download official installer
   - Extract installed binaries
   - Patch vboxdrv.sys directly (binary patching)
   - Avoids source build entirely

### The Memory Patch (Once Build Works)

**File 1: `src/VBox/VMM/VMMR3/PGM.cpp`** (~80 lines)
- Create shared memory file: `Global\VBoxHaywireMem` (Windows)
- Calculate total RAM size from VM configuration
- Use `CreateFileMapping()` for shared memory

**File 2: `src/VBox/Runtime/r0drv/nt/memobj-r0drv-nt.cpp`** (~40 lines)
- Modify `rtR0MemObjNativeAllocPage()`
- Instead of `MmAllocatePagesForMdl()`, allocate from shared memory
- Map shared memory into kernel address space

**Rebuild:** ~30 minutes (incremental, only modified files)

## Why VirtualBox on Windows is Hard

### VirtualBox is Developed on Linux

Oracle's team uses:
- **Primary development**: Linux (Debian/Ubuntu)
- **macOS builds**: For Mac-specific code
- **Windows builds**: Cross-compiled from Linux or built on controlled build servers

### Windows Build Challenges

1. **kBuild** - Designed for Unix, Windows support is secondary
2. **Environment assumptions** - No PATH length limits on Linux, no PROCESSOR_* vars
3. **Tool availability** - Qt6, various Unix tools more readily available on Linux
4. **Build infrastructure** - Oracle has dedicated VMs with exact tested configurations

### Our Situation

- Building natively on Windows 11
- Modern VS 2022 (VirtualBox tested mainly with VS 2019)
- Various Windows environment variables kmk doesn't expect
- PATH length limitations
- Missing optional components (Qt6, GSOAP) that may still be referenced

## Why We're Pivoting to QEMU

### QEMU Advantages

1. ✅ **Already built successfully** - You have qemu-mods/ working
2. ✅ **Simpler source** - Cleaner, more maintainable codebase
3. ✅ **Active development** - Better Windows support
4. ✅ **Can run Windows VMs** - qemu-system-x86_64.exe supports Windows guests
5. ✅ **No 4GB limitation** - The "4GB limit" was just `MEMSIZE="4G"` in launch script config

### The Real Problem (Not QEMU Limitation!)

**Previous understanding (WRONG):**
> "QEMU memory-backend-file stops at 4GB"

**Actual situation:**
- Launch scripts configure `-m 4G` (4GB guest RAM)
- memory-backend-file exposes the configured RAM (0-4GB)
- Linux kernel allocates PGDs at ~4.2-4.3GB physical addresses
- **These are OUTSIDE the RAM region** (in device/MMIO space)

**Solution:** Patch QEMU to expose full physical address space through memory-backend-file, not just RAM

### QEMU Patch is Simpler

- 1 file to modify (vs 2 for VirtualBox)
- Cleaner codebase to understand
- You already built it successfully
- Works for both Linux AND Windows guests

## Estimated Time Investment

### VirtualBox (Windows Build)

**Already spent:** ~8 hours
- Installing prerequisites: 2 hours
- Configuring: 2 hours
- Debugging build errors: 4 hours

**Still needed (estimate):**
- Fix "Invalid number" error: 2-8 hours (could be quick or very painful)
- Implement memory patch: 1 hour
- Test and debug: 2 hours
- **Total remaining:** 5-11 hours

### QEMU Patch (Alternative)

- Find memory-backend-file code: 30 minutes
- Understand mapping: 1 hour
- Implement patch: 1-2 hours
- Rebuild: 10 minutes
- Test: 1 hour
- **Total:** 3-5 hours

## Recommendation

**Pivot to QEMU patch.** Reasons:

1. ✅ **Already working** - QEMU builds successfully
2. ✅ **Less time** - 3-5 hours vs 5-11 hours
3. ✅ **More certain** - We know QEMU works, VirtualBox might hit more issues
4. ✅ **Better long-term** - QEMU is more actively developed
5. ✅ **Same goal** - Can introspect Windows x86_64 VMs either way

## Files to Keep

If we return to VirtualBox:

- `C:\Users\jamie\vbox-src\` - Complete source tree (configured)
- `configure_vbox.cmd` - Working configuration script
- `LocalConfig.kmk` - Build overrides (hardening disabled)
- Modified `configure.vbs` - VS 2022 support
- All build scripts (`build.cmd`, etc.)

**Disk space:** ~2-3GB

## Next Session Quick Start

If returning to VirtualBox build:

```cmd
cd C:\Users\jamie\vbox-src

# Try installing Qt6 first (might fix qt6.kmk parsing)
# Download from: https://www.qt.io/download-open-source
# Select: MSVC 2022 64-bit component

# Then try build again from Developer Command Prompt:
# Start Menu → "x64 Native Tools Command Prompt for VS 2022"
cd C:\Users\jamie\vbox-src
build_clean.cmd
```

Or try building driver only:
```cmd
kmk -C src/VBox/HostDrivers/Support
```

---

**Status:** On hold - Pivoting to QEMU patch approach

**Created:** November 4, 2025
**Last Updated:** November 4, 2025
