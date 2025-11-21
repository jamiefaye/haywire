# VirtualBox Source Code Reference

**Location**: `C:\Users\jamie\vbox-src`
**Repository**: https://github.com/mirror/vbox (mirror of official SVN)
**License**: GPLv2 (mostly) + CDDL for some components

## Overview

VirtualBox source cloned for understanding memory dump implementation and potentially accessing live VM memory via COM API.

## Key Directories for Haywire Integration

### Memory Management
- `src/VBox/VMM/` - Virtual Machine Monitor (core hypervisor)
  - `VMMR3/` - Ring-3 (userspace) VMM code
  - `VMMR0/` - Ring-0 (kernel) VMM code
  - `VMMAll/` - Code shared between rings

### Debugging & Core Dumps
- `src/VBox/VMM/VMMR3/DBGFCore.cpp` - **ELF core dump implementation**
  - This is where `dumpvmcore` is implemented
  - Creates ELF PT_LOAD segments for guest physical memory
  - Handles memory region enumeration

- `src/VBox/Debugger/` - Debugger UI and commands
  - `DBGCCommands.cpp` - Debugger console commands
  - Implements the `debugvm` command-line interface

### COM API
- `src/VBox/Main/` - COM/XPCOM interfaces
  - `MachineDebuggerImpl.cpp` - IMachineDebugger interface
  - Provides programmatic access to debugging functions
  - Could be used for live memory access from Windows

### Memory Access
- `src/VBox/VMM/VMMR3/PGM.cpp` - Physical Guest Memory management
  - Page management, physical address mapping
  - Understanding how VBox maps guest memory

## Relevant Files for Memory Introspection

### Core Dump Implementation
```
src/VBox/VMM/VMMR3/DBGFCore.cpp  - ELF dump creation
src/VBox/VMM/include/DBGFInternal.h - DBGF internals
```

### COM API for Live Access
```
src/VBox/Main/include/MachineDebuggerImpl.h
src/VBox/Main/src-client/MachineDebuggerImpl.cpp
```

### Memory Management
```
src/VBox/VMM/VMMR3/PGM.cpp - Physical memory
src/VBox/VMM/VMMR3/PGMR3Phys.cpp - Physical memory operations
```

## Understanding VirtualBox Memory Layout

From our dump analysis, VirtualBox exposes:

1. **Low Memory (0-4GB)**: Standard guest RAM
2. **High Memory (4GB+)**: Extended RAM regions
   - `0x100000000 - 0x120000000` (512MB) in our dumps
   - Contains kernel structures QEMU isolates

### Why VirtualBox Includes High Memory

Unlike QEMU's `memory-backend-file` which stops at configured RAM size, VirtualBox's `dumpvmcore`:
- Enumerates ALL physical memory regions
- Includes extended memory above 4GB boundary
- Creates PT_LOAD segment for each region
- Result: Complete physical address space in dump

## Key Questions to Answer (from source code)

1. **How does DBGFCore.cpp enumerate memory regions?**
   - Look for memory manager calls
   - How it decides what to include in dump

2. **Can we use COM API for live memory access?**
   - Check IMachineDebugger::readPhysical() method
   - Performance characteristics
   - Windows API usability

3. **Why does VBox include 4GB+ memory?**
   - Is it intentional or side effect?
   - Can it be disabled/controlled?
   - Security implications

## Next Steps

1. **Read DBGFCore.cpp** to understand ELF generation
2. **Check IMachineDebugger API** for live access
3. **Compare with QEMU** memory-backend-file approach
4. **Design Haywire VBox backend** based on findings

## Tools for Exploration

```bash
# Search for specific functionality
cd /c/Users/jamie/vbox-src
grep -r "dumpvmcore" src/VBox/VMM src/VBox/Debugger
grep -r "readPhysical" src/VBox/Main

# Find COM API definitions
find src/VBox/Main/idl -name "*.idl" | xargs grep -l Memory

# Examine memory management
cat src/VBox/VMM/VMMR3/PGM.cpp | less
```

## Documentation

- VirtualBox SDK: https://www.virtualbox.org/sdkref/
- COM API docs: https://www.virtualbox.org/sdkref/interface_i_machine_debugger.html
- Source organization: `doc/` directory in repo

## Notes

- VirtualBox uses kmk (kBuild Make) not standard Make
- Build system is complex - we don't need to build, just read
- Most relevant code is in `src/VBox/VMM/` and `src/VBox/Main/`
- COM API is cross-platform (XPCOM on Linux/macOS, COM on Windows)

---

**Purpose**: Understanding how VirtualBox provides better memory access than QEMU, and potentially building a native Windows Haywire using VBox COM API.
