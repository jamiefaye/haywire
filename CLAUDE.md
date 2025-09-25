# CLAUDE.md - AI Assistant Guide for Haywire Project

## Project Overview

Haywire is a VM memory introspection tool that bypasses QEMU's memory isolation to inspect kernel structures and process memory without guest cooperation.

**Current Status**: Transitioning from C++ implementation to web/JavaScript-based version for future development. The web version provides better visualization, cross-platform support, and easier deployment.

## Key Technical Context

### Memory Protection Discovery
- QEMU intentionally separates guest RAM from kernel structures
- Kernel page tables and task_structs are allocated beyond memory-backend-file boundaries
- This is a security feature, not a bug - it prevents casual host-level kernel inspection
- We bypass this using QMP commands with cpu_physical_memory_read()

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

### Legacy C++ Implementation (DEPRECATED)
- `companion-oneshot` - Current companion process (replaces beacon scheme)
- Previous beacon-based companions are obsolete

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

## Recent Progress (September 25, 2025)

### Swapper PGD Discovery
- **Dual approach**: QMP ground truth + adaptive signature search
- **Scoring algorithm**: Detects RAM size from PUD count, validates structure
- **100% accuracy**: Signature search confirms QMP ground truth
- **Memory efficient**: Optimized scanning prevents OOM errors
- **Works everywhere**: Electron (with QMP), browser mode, memory snapshots

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

## TODO/Future Work

### Immediate Tasks
- Complete migration from C++ to web-based implementation
- Remove obsolete beacon-based code and documentation
- Enhance web UI with discovered kernel information visualization

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