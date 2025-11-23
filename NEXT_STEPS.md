# Current State and Next Steps

## Just Committed (Slider Optimization)
- **What**: Optimized PA mode slider to skip PCI hole
- **How**: Slider represents file offsets (0-8GB) instead of physical addresses (0-10GB)
- **Benefit**: 25% better precision, no dead zone
- **Code**: `memory_visualizer.cpp` lines 1438-1455 (PA→file), 1707-1739 (file→PA), 1791-1820 (tooltip)

## Current Problem
- **Black hole below 2GB in PA mode** - lots of empty/unused RAM in lower 2GB
- **"hit buffer limit" messages** - QMP fallthrough when rendering crosses PCI hole boundary
- **Root cause**: Display code tries to read across PCI hole (PA 0x80000000-0x100000000) which doesn't exist in file

## Discovered Facts (from bug.log analysis)
- Windows x86-64 memory layout:
  - **ram-below-4g**: PA 0x0 - 0x7fffffff (2GB) → File offset 0x0
  - **PCI hole**: PA 0x80000000 - 0xffffffff (2GB) → **NOT IN FILE**
  - **ram-above-4g**: PA 0x100000000 - 0x27fffffff (6GB) → File offset 0x80000000
- Process discovery found **0 processes in lower 2GB, all processes above 4GB**
- Lower 2GB has non-zero data (firmware, page tables, ACPI) but no EPROCESS structures

## The Big Refactor Plan
**Goal**: Unify PA and VA modes using AddressSpaceFlattener

### Current Architecture (Dual Path)
- **VA mode**: Uses AddressSpaceFlattener with process VMAs → crunched address space
- **PA mode**: Raw physical addresses → hits PCI hole issues

### Proposed Architecture (Unified)
- **Both modes** use AddressSpaceFlattener
- **PA mode flattener**: Populated with RAM regions from MemoryMapper instead of VMAs
  - Region 1: Flat 0-2GB → PA 0x0-0x80000000 (ram-below-4g)
  - Region 2: Flat 2GB-8GB → PA 0x100000000-0x280000000 (ram-above-4g)
- **Benefits**:
  - Single rendering code path
  - PCI hole handled automatically by flattener (just a gap between regions)
  - No QMP fallthrough for hole addresses
  - Consistent slider behavior for both modes

### Implementation Steps
1. Modify AddressSpaceFlattener initialization to accept RAM regions in PA mode
2. Remove `if (useVirtualAddresses)` branching in rendering code
3. Update slider to always use flat addresses
4. Update CrunchedMemoryReader to work with PA regions
5. Test both PA and VA modes

### Key Files to Modify
- `include/address_space_flattener.h` - Add RAM region initialization
- `src/address_space_flattener.cpp` - Implement PA region population
- `src/memory_visualizer.cpp` - Remove PA/VA branching in rendering
- `src/memory_visualizer.cpp` - Update EnableVAMode() logic
- `include/crunched_memory_reader.h` - Ensure works for PA regions
- `src/memory_backend.cpp` - May need adjustments for region queries

### Risks
- Large refactor touching many code paths
- Both PA and VA modes must continue working
- Heat map, change detector, mini viewers all depend on this

### Rollback Point
- Just committed slider optimization
- Can `git reset --hard HEAD` if refactor goes wrong
