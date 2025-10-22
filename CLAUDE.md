# CLAUDE.md - AI Assistant Guide for Haywire Project

## Project Overview

Haywire is a VM memory introspection tool that bypasses QEMU's memory isolation to inspect kernel structures and process memory without guest cooperation.

**Current Status**: Both C++ and web implementations are actively maintained. C++ version provides native performance with live change detection and heat map visualization. Web version offers cross-platform support and easier deployment.

## Key Technical Context

### Memory Protection Discovery
- QEMU intentionally separates guest RAM from kernel structures
- Kernel page tables and task_structs are allocated beyond memory-backend-file boundaries
- This is a security feature, not a bug - it prevents casual host-level kernel inspection
- We bypass this using QMP commands with cpu_physical_memory_read()

### Memory Access Methods
- **Primary**: Memory-mapped file with MAP_SHARED (`/tmp/haywire-vm-mem`)
  - Provides instant access to live QEMU memory updates
  - Critical: Must use MAP_SHARED, not MAP_PRIVATE (which creates static snapshot)
  - Used for display, change detection, and all performance-critical paths
- **Secondary**: QMP commands for kernel structures outside RAM bounds
  - Only needed for memory beyond memory-backend-file boundaries
  - Used sparingly due to performance overhead

### Important Memory Addresses (ARM64 Ubuntu)
- Guest RAM: 0x40000000 to configured size (2GB/4GB/6GB)
- Kernel structures with highmem=on: ~0x1b4dbf000 (6.8GB)
- Kernel structures with highmem=off: ~0xb11bf000 (2.77GB)
- Both are outside memory-backend-file scope

## Project Structure

### Web Implementation (JavaScript/TypeScript) - PRIMARY
- `web/src/kernel-discovery-paged.ts` - Main kernel discovery with paged memory support
- `web/src/kernel-discovery.ts` - Core discovery algorithms and types
- `web/src/paged-memory.ts` - Efficient large file handling
- `web/src/components/` - Vue.js UI components
- `web/src/electron/` - Electron-specific functionality (QMP access)

### C++ Implementation (Native)
- Direct memory-mapped file access via `MemoryFileReader` with MAP_SHARED
- `KernelViewportTranslator` - VA→PA translation using kernel discovery
- `KernelDiscoveryBackend` - Process discovery and PGD extraction
- `ChangeDetector` - Background patrol thread for change detection
- `HeatMapWidget` - Real-time memory change visualization
- No companion process needed - all discovery done internally

### Test Scripts
- `test_*.mjs` - Node.js test scripts for validation
- `test_swapper_*.mjs` - Swapper PGD discovery validation
- Python scripts (`.py`) - Legacy research scripts

### QEMU Modifications
- `qemu-mods/` - Modified QEMU source
- `qemu-mods/qemu-src/qapi/misc.json` - QMP command definitions
- `qemu-mods/qemu-src/target/arm/arm-qmp-cmds.c` - Implementation

### Documentation
- `docs/memory-map-visual.md` - Physical memory layout and mapping diagram (CURRENT)
- `docs/rendering_pipeline.md` - Memory rendering pipeline and column mode (CURRENT)
- `docs/address_notation.md` - Address notation system (CURRENT)
- `docs/vm_setup_guide.md` - VM setup instructions (CURRENT)
- `docs/build_qemu.md` - Building modified QEMU (CURRENT)
- Other docs may be obsolete - verify before use

## Common Tasks

### Testing Memory Access
```bash
# Start VM with highmem=off (from scripts dir)
./launch_ubuntu_highmem_off.sh

# Test kernel memory access
python3 test_highmem_off.py
```

### Building Modified QEMU
```bash
cd qemu-mods/qemu-src
mkdir build && cd build
../configure --target-list=aarch64-softmmu
make -j8
```

### Running Custom QMP Commands
```python
import socket
import json

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 4445))
sock.recv(4096)  # banner
sock.send(json.dumps({"execute": "qmp_capabilities"}).encode() + b'\n')
sock.recv(4096)

# Our custom command
cmd = json.dumps({"execute": "query-kernel-info", "arguments": {"cpu-index": 0}})
sock.send(cmd.encode() + b'\n')
response = json.loads(sock.recv(4096).decode())
```

## Important Warnings

### Security
- NEVER run sensitive workloads in Haywire-monitored VMs
- This tool completely breaks VM isolation
- Exposes kernel keyrings, credentials, encryption keys

### Compatibility
- Primary target: Linux on ARM64/x86-64
- Windows support planned but not implemented
- macOS guest support unlikely due to hypervisor framework

## Design Decisions

### Why Not Use Guest Agent?
- Can be disabled/compromised by guest
- Requires guest cooperation
- Doesn't work for forensics/incident response

### Why Python for Tests?
- Rapid prototyping
- Easy QMP interaction
- Production code is C++ for performance

## Key Insights

1. **highmem=off doesn't help** - Kernel structures still hidden
2. **Memory isolation is architectural** - Not a configuration issue
3. **QMP bypass works** - cpu_physical_memory_read() accesses everything
4. **Guest OS agnostic** - Let QEMU handle MMU complexity
5. **malloc'd memory visible** - Regular heap IS visible through memory-backend-file
6. **tmpfs NOT visible** - /dev/shm doesn't appear in memory-backend-file
7. **Page alignment critical** - Beacons must be on 4KB boundaries
8. **Zero page optimization** - memset(0) doesn't allocate physical pages

## Recent Progress (August 2025)

### PID to PGD Mapping Success
- **Fixed maple tree walker**: Using kernel's exact node type detection `(ptr >> 3) & 0x0F`
- **Leaf vs internal nodes**: Types 0-1 are leaves (contain VMAs), 2-3 are internal (contain children)
- **Live memory critical**: Snapshot had 21% success, live memory has 100% success rate
- **Successfully extracted**: 39/39 user process PGDs from live memory
- **Key offsets confirmed**:
  - `mm_struct.pgd` at offset 0x68
  - `mm_struct.mm_mt` (maple tree) at offset 0x40
  - Maple tree root at offset 0x48

### Process Discovery Methods
- **Memory scan**: Finds task_structs by scanning memory for signatures
- **Filters**: Strict validation of process names, PIDs, linked lists
- **Tombstones**: Snapshot contained stale processes, live memory only has active ones
- **Ground truth**: GuestAgent class can get `ps aux` via QGA for comparison

### Kernel Structure Offsets Needed for Portability
- **task_struct**: pid, comm, mm, tasks list
- **mm_struct**: pgd (0x68), mm_mt (0x40), mm_users
- **vm_area_struct**: vm_start (0x00), vm_end (0x08), vm_flags
- **Detection methods**: BTF/pahole, kallsyms, debug symbols, heuristics

## Recent Progress (September 2025)

### Address Notation System
- Unified notation for specifying addresses across memory spaces
- Four address spaces: `s:` (shared), `p:` (physical), `v:` (virtual), `c:` (crunched/flattened)
- Expression support with arithmetic: `s:1000+200`, `p:40000000-0x100`
- Full documentation in `docs/address_notation.md`
- Status bar now displays addresses in all relevant spaces

### Mini Bitmap Viewers
- Floating bitmap viewer windows that can be spawned from main memory view
- Right-click context menu to create viewers at any memory location
- Leader lines connect viewers to their source location in main memory
- Full pixel format support (11 formats including new ones below)
- Dynamic address updates when anchors are dragged
- VA mode support with proper address translation via CrunchedMemoryReader

### New Display Formats
- **Hex Pixel**: 4-byte chunks displayed as 8 hex digits in 32x8 pixel cells
- **Char 8-bit**: Single bytes displayed as character glyphs using 6x8 pixel font
- **Split Components**: RGB/RGBA pixels split into individual color channels
  - Shows components in memory order with natural colors
  - Available in both main visualizer and mini viewers
  - Toggled via "Split" option in format selector

### Column Mode Memory Layout
- **Multi-column display**: Memory flows vertically down columns, then wraps to next column
- **Simplified controls**: Only column width and gap (height = window height, stride = width)
- **Unified pixel units**: All widths use pixels for consistency (not mix of bytes/pixels)
- **Proper coordinate transformations**: Mouse clicks, change tracking, mini-viewers all work correctly
- **Full documentation**: See `docs/rendering_pipeline.md` for implementation details

### UI Improvements
- Default width changed from 640 to 1024 pixels
- Compact "Split" label instead of "Split Components" 
- Dynamic combo box height to show all format options
- Format selector moved left in mini viewers for narrow window access
- Address-based labels for bitmap viewers (e.g., "s:0x1000" instead of "Viewer 1")
- Dynamic control bar height that adjusts for column mode (3 rows vs 2 rows)
- Select button in memory visualizer opens Process Selector window
- Removed redundant Process Selector [P] button (kept hotkey)

### Companion Process (C++ - Legacy)
- **Current**: `companion-oneshot` - Single-shot process information gatherer
- **Deprecated**: Previous beacon-based architecture (beacon_encoder/decoder)
- **Note**: C++ companion approach is being phased out in favor of web-based discovery

### SSH Setup
- Primary user: `ubuntu` (passwordless via SSH key)
- Emergency user: `jff` with password `p`
- Host alias: `vm` (localhost:2222)
- See `docs/vm_setup_guide.md` for complete setup

## Web UI Features (September 2025)

### Change Detection System
- **SIMD-optimized memory scanning**: Uses WebAssembly SIMD (wasm_simd128) for performance
- **Incremental scanning**: Processes 6.4MB per frame to handle large files without freezing UI
- **Visual change indicators**: Green for changed chunks, dark gray for zeros, blue gradient for data
- **Opt-in feature**: Disabled by default to avoid user confusion
- **64KB chunk granularity**: Balances performance vs precision
- **Checksum-based tracking**: Rotation-based mixing for better collision resistance
- **Overview pane visualization**: Memory map showing change patterns at a glance

### Browser Limitations Discovered
- **File System Access API issues**:
  - Permissions revoked when files modified externally
  - Cannot monitor shared files without user gesture
  - No true file watching capability in browsers
- **WebSocket requirement for QEMU**: Direct TCP connections blocked by browser security
- **Workarounds**:
  - Manual file re-open required after external modifications
  - Browser reload counts as "user gesture" for permissions
  - Future Electron version will bypass these limitations

### WASM Module Architecture
- Consolidated change detection into existing `memory_renderer.js` module
- Functions added: `testChunkZeroSIMD`, `calculateChunkChecksumSIMD`
- Specific optimized versions for common sizes (4KB, 64KB, 1MB)
- Compiled with Emscripten `-msimd128` flag for SIMD support

## Recent Progress (September 30, 2025)

### VA Mode Heat Map Performance Optimization
**Problem**: Heat map patrol in VA mode was extremely slow (10+ seconds to scan 2037 chunks vs instant in PA mode)

**Root Cause Discovered**:
- VA mode was doing ~32,000 page table walks per heat map scan (16 walks per 64KB chunk)
- Each chunk took 6-7ms average (vs <1us in PA mode)
- Unmapped pages were going through expensive `ReadCrunchedMemory()` path unnecessarily

**Solution Implemented**:
1. **Lazy PA Lookup Table**:
   - Allocates 5MB lookup table instantly (no 30-second startup delay)
   - Translates VA→PA on first access only, caches result
   - Subsequent accesses use cached PA (instant array lookup)

2. **Zero-Copy Direct Pointer**:
   - Added `GetDirectPointer(flatAddr)` to `CrunchedMemoryReader`
   - Uses PA lookup → returns pointer to MAP_SHARED mmap'd memory
   - Eliminates memcpy overhead (was copying 64KB per chunk)

3. **Fast Unmapped Page Handling** (KEY FIX):
   - Detects unmapped pages via PA lookup returning 0
   - Treats them as all-zeros instantly instead of expensive translation attempts
   - This fixed 84% of chunks that were unmapped and going through 6-7ms slow path

**Results**:
- Heat map patrol now completes in **1-2 seconds** (vs 10+ seconds before)
- Nearly matches PA mode performance
- Startup is instant (no pre-translation of 655K pages)
- Pages translated lazily as accessed, cached for subsequent scans

**Implementation Details**:
- `AddressSpaceFlattener::EnableLazyPALookup()` - allocates table without translating
- `AddressSpaceFlattener::GetPhysicalAddress()` - lazy translation on cache miss
- `CrunchedMemoryReader::GetDirectPointer()` - zero-copy access via PA lookup
- `ChangeDetector::ScanChunk()` - fast path for unmapped pages (treat as zeros)

**Files Modified**:
- `include/address_space_flattener.h` - added PA lookup table and lazy translation
- `src/address_space_flattener.cpp` - implemented lazy lookup and GetPhysicalAddress()
- `include/crunched_memory_reader.h` - added GetDirectPointer()
- `src/crunched_memory_reader.cpp` - implemented zero-copy pointer access
- `src/change_detector.cpp` - fast unmapped page detection
- `src/memory_visualizer.cpp` - wired up lazy PA lookup on process selection

## Recent Progress (December 2025)

### Important Discovery: ARM64 + ASLR Memory Layout
- **OLD ASSUMPTION (WRONG)**: User space uses PGD indices 0-255, kernel uses 256-511
- **REALITY WITH ASLR**: User processes use PGD indices throughout 0-511!
  - Example: VLC at 0xc3048ea20000 uses PGD index 390
  - Most processes start at 0xe... or 0xf... addresses (PGD indices 448-511)
  - This is due to ASLR placing processes high in the 48-bit address space for security

### Actual Memory Layout
- **User Space**: 0x0000000000000000 - 0x0000FFFFFFFFFFFF (48-bit addresses)
  - With ASLR enabled: Processes placed at HIGH addresses
  - Common ranges: 0xc3..., 0xe7..., 0xf1... (PGD indices 390, 463, 482, etc.)
- **Kernel Space**: 0xFFFF000000000000 - 0xFFFFFFFFFFFFFFFF (bits 63-48 all set)
  - Identified by top 16 bits being 0xFFFF

### PGD Index Mapping
- Each PGD entry covers 512GB (2^39 bytes)
- PGD index = bits [47:39] of virtual address
- Example: 0xc3048ea20000 >> 39 = 390 (PGD index)

## Recent Progress (September 29-30, 2025)

### Critical Memory Mapping Fix - MAP_SHARED
- **Problem**: Memory was mapped with `MAP_PRIVATE`, creating a copy-on-write snapshot
- **Impact**: Heat map couldn't see live QEMU updates, required slow QMP calls for fresh data
- **Solution**: Changed `MemoryFileReader` to use `MAP_SHARED` in `memory_file_reader.cpp`
- **Results**:
  - Instant access to live memory updates from QEMU
  - Heat map now sees real-time changes automatically
  - Dramatically improved refresh performance (matches web version speed)
  - All memory access now uses fast mmap instead of slow QMP calls

### Heat Map Implementation
- **Change Detection**: Background patrol thread scans memory for changes
  - Priority 1: Visible chunks at 20 Hz (50ms interval)
  - Priority 2: Heat map area at 4 Hz (250ms interval)
  - Priority 3: Background random sampling (10 chunks/cycle)
  - Dynamic scan limits based on heat map size
- **Color Coding**:
  - Green: just changed
  - Yellow: recent changes
  - Orange: older changes
  - Red: very old changes
  - Blue: stable (no changes)
  - Dark gray: zero pages
  - Black: unscanned
- **Smart Positioning**: Heat map centers on viewport (top/middle/bottom depending on scroll position)
- **Yellow Indicator**: Shows current viewport location within heat map context

### Slider Range Mapping Fix
- **Problem**: Slider covered 0-8GB physical space, but RAM starts at 0x40000000 (1GB offset)
- **Solution**:
  - Slider now represents file offsets (0 to RAM size)
  - Converts to/from physical addresses (file_offset + 0x40000000)
  - Uses `MemoryMapper` to get actual RAM region from QEMU
- **Result**: Slider position 0 shows RAM start, not empty boot ROM area

### Performance Improvements
- No more QMP calls for memory display (all via mmap)
- Smooth real-time updates without lag
- CPU-friendly scan rates (20Hz visible, 4Hz heat map)
- Full heat map coverage without throttling

## Recent Progress (September 25-29, 2025)

### Swapper PGD Discovery - IMPROVED SCORING ALGORITHM
- **Problem identified**: Original scoring often selected wrong PGD candidate
- **Root cause**: Swapper has only 1 user entry (PGD[0]), processes have many
- **Solution**: Heavily weight single user entry as swapper signature (100 points)
- **Improved scoring**:
  - 1 user entry = +100 points (swapper signature!)
  - 2 user entries = +50 points
  - 4+ user entries = +20 points
  - 16+ user entries = -50 points (likely process PGD)
  - Must have PGD[256] for kernel text (+15/-20 points)
  - Kernel entries should be 2-20 (+20 points)
- **Implementation**: Both TypeScript (kernel-discovery-paged.ts) and C++ (kernel_discovery.cpp)
- **Testing status**: Implementations complete, testing pending VM restart
- **Files modified**:
  - `web/src/kernel-discovery-paged.ts`: analyzeSwapperCandidate() function
  - `src/kernel_discovery.cpp`: FindSwapperPGD() and AnalyzeSwapperCandidate() functions

## Recent Optimizations (September 14, 2025)

### Memory Scanning Performance
- **Zero-copy page scanning**: Added `TestPageNonZero` methods to avoid memory allocation
- **PA mode performance**: 45.4ms → 7.8ms for 10k pages (5.8x speedup)
- **VA mode performance**: 60ms → 7ms for 1k pages (8.5x speedup)
- **64-bit OR accumulation**: Process 8 bytes at once with loop unrolling
- **Increased scan ranges**: PA mode 30k pages (120MB), VA mode 3k pages (12MB)
- **Auto-repeat scanning**: 500ms initial delay, 20Hz repeat rate

### Architecture Improvements
- Unified memory access through QemuConnection for display and scanning
- CrunchedMemoryReader now supports zero-copy TestPageNonZero
- Smart region skipping in PA mode to avoid unmapped memory

## Recent Progress (September 28-29, 2025)

### Complete Beacon/Companion Removal
- **Removed beacon architecture**: No more beacon_reader, beacon_decoder
- **Replaced with kernel discovery**: Direct PTE extraction from discovered processes
- **New components**:
  - `MemoryFileReader`: Simple mmap wrapper for memory file access
  - `KernelViewportTranslator`: Uses kernel discovery for VA→PA translation
  - `KernelDiscoveryBackend`: Manages process discovery and PGD extraction
- **Benefits**: No guest cooperation needed, works with snapshots, more reliable
- **Status**: Migration complete, all bitmap viewers and crunched reader working

## Recent Progress (September 26, 2025)

### Major Bug Fixes
- **Fixed mm_users offset**: Was reading from 0x38, correct offset is 0x74 (verified via pahole)
- **Fixed translateVA special case bug**: Removed incorrect 0xffff0000 routing through PGD[0]
- **Fixed maple tree walking**: Now correctly walks even with mm_users=0
- **VLC memory maps now working**: Shows correct filenames and addresses

### UI/UX Improvements
- **Kernel discovery caching**: Results cached after first run, instant subsequent lookups
- **Shift-key tooltips**: Tooltips only appear when Shift is held (reduced spam)
- **Fixed memory maps display**: Removed 26-space gap, removed meaningless "00:00 0"
- **Added refresh button**: Manual refresh in kernel discovery modal

### Code Cleanup
- **Removed 450+ debug statements**: Only 9 essential console.logs remain
- **Organized test files**: All test scripts moved to test_attempts/
- **Cleaner memory maps**: Simplified format without device/inode info

## Upcoming: Memory View Mapping System

### Goal
Create flexible sorting/filtering of memory pages without moving data, using mapping tables for indirection.

### Architecture
```typescript
class MemoryMapping {
  displayToFile: number[]              // Forward: display index → file page index
  fileToDisplay: Map<number, number[]> // Reverse: file → multiple display positions
}
```

### Implementation Plan
1. **Phase 1**: Linear identity mapping (NOP) - no visual change
2. **Phase 2**: Test with reverse mapping (highest address at top)
3. **Phase 3**: Sort by PID + Virtual Address
4. **Phase 4**: Add crunching to remove gaps
5. **Phase 5**: Complex views (group by type, security analysis, etc.)

### Key Challenges
- Many-to-one mappings (shared memory/libraries)
- One-to-many reverse lookups
- Non-linear mouse/scroll behavior
- Rendering discontinuous memory regions efficiently

## Recent Progress (October 1, 2025)

### Firefox Process Discovery Fix
- **Problem**: Firefox processes (PIDs 15520-16193) not appearing in PID selector
- **Root Cause**: Process name validation rejected space and tilde characters
- **Solution**: Added space (`' '`) and tilde (`'~'`) to allowed characters in `CheckTaskStruct()`
- **Result**: Firefox thread names like "Utility Process" and "AudioIP~allback" now discovered
- **File Modified**: `src/kernel_discovery.cpp` - updated character validation pattern

### Inspector Mini Dump Memory Value Fix
- **Problem**: Mini dump showing incorrect memory values (often zeros) instead of actual bytes
- **Root Cause**: Byte offset calculation didn't account for column mode and rendering transformations
- **Solution**:
  - Created `PixelCoordToByteOffset()` helper that uses `GetAddressAt()` logic
  - Properly handles column mode, split components, and all pixel format transformations
  - Converts display pixel coordinates back to byte offsets in `currentMemory.data`
- **Result**: Mini dump now shows actual underlying memory bytes at cursor/magnifier position
- **Files Modified**:
  - `include/memory_visualizer.h` - added helper declaration
  - `src/memory_visualizer.cpp` - implemented coordinate-to-byte conversion

### Search Navigation to Black Pages Fix
- **Problem**: Searching for patterns landed on unmapped/black pages instead of actual matches
- **Root Causes**:
  1. Full-range search stored file offsets (0 to 6GB) instead of addresses
  2. `ScrollToResult()` used pixels instead of bytes for stride calculations
  3. Byte-to-pixel conversion for magnifier positioning was incorrect
- **Solutions**:
  1. Convert file offsets to proper addresses:
     - PA mode: `file_offset + 0x40000000` (RAM base)
     - VA mode: `file_offset` = flat address (already correct)
  2. Fixed `bytesPerLine = viewport.stride * viewport.format.bytesPerPixel`
  3. Fixed byte-to-pixel conversion to handle column mode correctly
- **Result**: Search now lands on actual matches, magnifier locks correctly, Next/Previous work
- **Files Modified**: `src/memory_visualizer.cpp` - search addressing and navigation

### VM Disk Management and Backup Strategy
- **Discovered**: Currently using direct write to `ubuntu_arm64.qcow2` (not overlay mode)
- **Overlay Status**: `ubuntu_arm64_overlay.qcow2` was outdated/invalid - discarded
- **Backups Created**:
  - Pre-cleanup: `backups/ubuntu_arm64_pre_updates_20251001.qcow2` (8.1GB compressed)
  - Post-cleanup: `backups/ubuntu_arm64_post_cleanup_20251001.qcow2` (9.1GB compressed)
- **Disk Cleanup**: Freed 2GB by removing Linux source and old headers
  - Removed `/usr/src/linux-source-6.14.0` (1.8GB)
  - Removed old kernel headers 6.14.0-32 (208MB)
  - Cleaned package cache (785MB)
  - **Verified**: BTF/pahole still works (reads from `/sys/kernel/btf/vmlinux`, not source)
- **Result**: VM now has 3.6GB free (was 922MB) for installing applications like Blender
- **Disk Usage**: Went from 95% full (17GB used) to 81% full (15GB used)

### BTF/Pahole Requirements Clarified
- **Discovery**: BTF data is embedded in running kernel at `/sys/kernel/btf/vmlinux` (7.4MB)
- **No Source Needed**: `pahole` reads from running kernel, NOT from source or headers
- **Safe to Remove**: Linux source and headers can be removed without affecting kernel introspection
- **Use Case**: Source/headers only needed for kernel development or building modules

## TODO/Future Work

### Immediate Tasks
- Implement memory mapping system for flexible page sorting/filtering
- Update all rendering code to use mapping indirection
- Add UI controls for different view modes (by PID, by type, crunched, etc.)

### Medium-term Goals
- Support different kernel versions with offset configuration files
- Windows guest support via EPROCESS structures
- Implement process memory dumping via discovered PGDs
- Add PTE analysis and memory mapping visualization
- Create process tree visualization from parent/child relationships

### Long-term Vision
- Multi-VM support with synchronized views
- Automatic kernel version detection and offset discovery
- Integration with debugging tools (GDB, LLDB)
- Live memory diffing between snapshots


## Debugging Tips

- Use `fprintf(stderr, ...)` in QEMU code for debugging
- Check `/tmp/kernel_dump.txt` for QMP dump output
- Memory file is at `/tmp/haywire-vm-mem` (memory-backend-file)
- QMP port is usually 4445, monitor port 4444
- Kill background processes with: `killall ssh haywire`

## Contact/Issues

File issues in the GitHub repository. This is a research project - use at your own risk!
## Recent Progress (October 22, 2025)

### Critical VA Mode Rendering Fixes

**PageDatabase Query System:**
- Multi-PID page tracking with wildcard name queries (e.g., `vlc*`, `*firefox*`)
- Thread-safe query results using PageMetadata copies instead of pointers
- Fixed race condition where background scanner could modify results during display
- Deduplication and sorting options for shared pages
- Generation-based color progression (Green→Cyan→Purple→Orange) for rescan feedback
- Faster first scan (10ms/PID) with CPU-friendly rescans (100ms/PID)

**Mini Bitmap Viewer Fixes:**
- Fixed PID 0 support for query results (was checking `currentPid > 0`)
- Fixed `EnableVAMode()` not updating bitmap viewer manager with crunched reader
- Mini viewers now work correctly for both PID selector and query results
- Removed test pattern fallback when crunched reader was available but not wired up

**VA Mode Buffer Reading - Critical Bugs Fixed:**

**Bug #1: Offset Advancement**
```cpp
// OLD (WRONG):
chunkSize = min(pageSize, size - offset);  // e.g., 100 bytes
memcpy(buffer + offset, ptr, chunkSize);
offset += pageSize;  // ❌ Skipped 3996 bytes! Left gaps!

// NEW (CORRECT):
offset += chunkSize;  // ✅ Advance by actual bytes copied
```
Result: Buffer had uninitialized gaps causing visual artifacts

**Bug #2: Page Boundary Clipping**
```cpp
// GetDirectPointer returns pointer valid only to end of current page
// If flat addr = 4094 (2 bytes from page end), pointer only valid for 2 bytes!

// NEW: Clip to page boundary
offsetInPage = flatAddr % 4096;
bytesLeftInPage = 4096 - offsetInPage;
chunkSize = min(bytesLeftInPage, bytesNeeded);
```
Fixed in three call sites:
- `memory_visualizer.cpp`: Viewport rendering (307KB buffer fills)
- `change_detector.cpp`: Heat map chunk scanning
- `qemu_connection.cpp`: Already safe (page-aligned calls only)

**Key Insight from User:**
- "Stride should equal width" - no fancy alignment needed
- Rendering naturally handles pixels spanning page boundaries
- Buffer just needs to be complete and contiguous (gaps filled with zeros)
- RGB pixels can straddle pages as long as buffer has all bytes in order

**Impact:**
- Eliminated visual artifacts in VA mode (black stripes, corrupted rows)
- Complete, contiguous buffers with proper zero-filling for unmapped pages
- 3-byte formats (RGB/BGR) now render correctly across page boundaries
- Mini viewers work reliably for query results
- Heat map scans don't corrupt checksums with garbage data

## Recent Progress (October 2, 2025)

### Instruction Disassembly and Visualization

**Capstone Integration:**
- Enabled Capstone 5.0.6 disassembler library
- Works on macOS (Homebrew) and Ubuntu ARM64 (`libcapstone-dev`)
- Proper CMake integration with library path detection
- Currently using CS_OPT_DETAIL OFF for basic disassembly
- Could enable detailed mode for branch target extraction, register info, etc.

**InstructionRenderer Class:**
- Disassembles ARM64 instructions (32-bit, 4-byte aligned)
- Categorizes into 10 types: move, arithmetic, logic, load, store, branch, compare, SIMD/FP, system, invalid
- Graceful garbage handling - shows `.word HEXVALUE` for non-code data
- Color coding: Blue (move), Yellow (arithmetic), Green (load), Orange (store), Red (branch), Brown (data)
- Icon glyphs: 4×6 pixel bitmaps for each category
- UDF instructions (0x00000000) rendered nearly invisible (0x0A0A0A dark gray)

**Compact View (40px × 8px):**
- 40px × 8px per instruction (perfect for column mode)
- Layout: [6px colored icon][34px operands in 3×5 font]
- Recommended viewing: 40px column width + gap pixels for nice ARM64 code display
- Data regions show as `.word 12345678` instead of `???`
- Brown checkerboard icon distinguishes data from code

**Format Menu Updates:**
- Added ARM64 Insn to both main visualizer and bitmap viewer format menus
- Cleaned up format labels to match web version (RGB instead of RGB888, etc.)
- Split variants shown as pipe-separated (R|G|B|A, B|G|R|A, etc.)
- Removed old "Split" checkbox in favor of explicit split format options

### Magnifier/Inspector Window Improvements

**Layout Optimization:**
- Removed help text section at bottom for more magnification space
- Hex data toggle moved to top control bar (next to Lock checkbox)
- Compact "Hex" checkbox instead of full-width button at bottom
- Window properly sizes without scrollbars (520px base, +50px with hex)
- Fixed: Window now resizable vertically (was locked by constraints)

**Hex Data Section:**
- Collapsible section controlled by "Hex" checkbox
- Starts collapsed by default (max space for magnification)
- When enabled, expands window by 50px (tight fit, no wasted space)
- Shows 3 lines of hex dump (above, current, below) with ASCII
- Format-specific value display (RGB, RGBA, BGR, etc.)

**User Experience:**
- No scrollbars - everything visible from start
- Hex data accessible via single checkbox click
- Window grows/shrinks smoothly when hex toggled
- User can manually resize window at any time

### Performance and Build Options

**Build Modes:**
- Debug mode: Standard development build with full debug symbols
- RelWithDebInfo: Full optimizations (-O2/-O3) + debug symbols (~15% FPS boost)
- Release: Maximum optimization, no debug info
- Easy switching: `cmake -DCMAKE_BUILD_TYPE=<mode> build && cmake --build build`

### Future Considerations

**Light Theme:**
- Would change GUI chrome only (windows, buttons, text)
- Memory rendering stays same (0x00=black, 0xFF=white pixel values)
- Requires color tweaking for status bars, autocorrelation, etc.
- ImGui makes basic toggle easy, but custom colors need adjustment

**Advanced Disassembly Features (Future):**
- Branch target extraction (enable CS_OPT_DETAIL for operand details)
- Symbol table integration (would need ELF/Mach-O parser like LIEF)
- Control flow arrows (breaks current rendering model, would need overlay layer)
- Multiple scales: detailed (120px), gist (60px), current compact (40px)
- Pattern recognition via color strips showing instruction flow
- For now: Color-coded compact view works well for scanning code patterns

### Autocorrelation Improvements

**Pixel-Format-Aware Sampling:**
- Matches old Haywire implementation for better display quality
- Handles all 11 pixel formats with format-specific extraction
- Proper normalization (÷32768, then ÷16384) matches original
- Auto-scaling based on actual max correlation value
- Relative peak threshold (30% of max) for adaptive detection

**Key Insight:**
- Old implementation had sophisticated per-format sample extraction
- Current version now matches this with proper 8/16/24/32-bit handling
- Produces more satisfying autocorrelation displays

