# Haywire Beacon Architecture - Current State
*Last Updated: September 5, 2025*

## Overview

Haywire is a VM memory introspection tool that uses a cooperative beacon-based protocol to expose kernel information that QEMU intentionally hides from the memory-backend-file (for security). 

**Key Components:**
- **Companion** - Process running inside the VM that reads kernel info and publishes it via beacons
- **Beacons** - 4KB aligned memory pages with magic headers, visible in memory-backend-file
- **Haywire** - Host-side reader that mmaps memory-backend-file and interprets beacons

## Why Beacons?

QEMU intentionally prevents direct access to kernel structures (page tables, task_structs) through memory-backend-file as a security feature. These structures exist at addresses beyond the configured guest RAM boundaries. Beacons work around this by having code inside the VM read this information via /proc and publish it in regular heap memory that IS visible through memory-backend-file.

## Shared Protocol

The file `include/beacon_protocol.h` defines the shared data structures between companion (C code in VM) and Haywire (C++ code on host). This ensures both sides agree on:

- Page structures (all exactly 4096 bytes)
- Magic numbers (0x3142FACE primary, "HayD" for discovery)
- Beacon categories and their purposes
- Process and memory section data formats

## Beacon Categories

The system uses 5 beacon categories:

1. **MASTER (0)** - Discovery pages and control
   - 10 pages allocated
   - First page is discovery page with "HayD" magic
   
2. **ROUNDROBIN (1)** - Round-robin process scanning
   - 500 pages allocated
   - Cycles through all processes, capturing detailed info
   
3. **PID (2)** - PID list snapshots
   - 100 pages allocated
   - Quick snapshots of all PIDs in system
   - Keeps 10 generations for history
   
4. **CAMERA1 (3)** - Focused monitoring of specific PID
   - 200 pages allocated
   - Detailed inspection including PTEs
   
5. **CAMERA2 (4)** - Second camera for another PID
   - 200 pages allocated
   - Allows monitoring two processes simultaneously

## Memory Discovery Process

1. **Companion allocates beacons** - Uses malloc() to get pages, marks with magic
2. **Haywire scans entire memory** - Checks first word of each 4KB page for 0x3142FACE
3. **Builds mapping arrays** - For each category, maps logical index → physical page number
4. **Direct access thereafter** - O(1) lookup using mapping arrays

### Mapping Arrays

Haywire maintains 5 mapping arrays:
```c
uint64_t master_mapping[BEACON_MASTER_PAGES];      // 10 entries
uint64_t roundrobin_mapping[BEACON_ROUNDROBIN_PAGES]; // 500 entries  
uint64_t pid_mapping[BEACON_PID_PAGES];            // 100 entries
uint64_t camera1_mapping[BEACON_CAMERA1_PAGES];    // 200 entries
uint64_t camera2_mapping[BEACON_CAMERA2_PAGES];    // 200 entries
```

Each entry maps: `beacon_index → physical_page_number_in_memfile`

## Camera Focus Protocol

The only time Haywire writes to memory-backend-file:

1. **Read** camera control page, verify magic/structure
2. **Write** new target_pid to control page
3. **Read back** to verify write succeeded
4. **Bail** if verification fails (indicates memory pressure/eviction)

This careful protocol ensures we don't corrupt VM memory if pages get swapped.

## Data Captured

### Process Information
- PID, PPID, UID, GID
- Process name (comm)
- State (R/S/D/Z/T)
- Memory usage (vsize, RSS)
- CPU times
- Executable path

### Memory Sections (from /proc/PID/maps)
- Virtual address ranges
- Permissions (rwxp)
- Mapped files
- Special regions ([heap], [stack], etc.)

### Page Table Entries (from /proc/PID/pagemap)
- RLE compressed to fit more data per beacon
- Physical frame numbers
- Present/swapped status

## Companion Variants

Current companion implementations:

- **companion_camera.c** - Main implementation using shared protocol
- **companion_roundrobin.c** - Round-robin process scanner
- **companion_pid.c** - PID list generator
- **companion_selftest.c** - Testing beacon discovery
- **companion_benchmark.c** - Performance testing
- **companion_multi.c** - Multiple category test
- **companion_simple_test.c** - Minimal beacon test

## Current Issues & Future Work

### Completed
- ✅ Removed stale beacon_types.h (old design)
- ✅ Removed beacon_scanner.cpp (used old header)
- ✅ Established beacon_protocol.h as single source of truth

### Known Issues
- Discovery page exists but doesn't contain hints/offsets (requires full scan)
- No index for O(1) initial discovery (must scan all memory)
- Companion variants have duplicate code (should share more)

### Future Improvements
1. **Master index page** - Add beacon offsets to discovery page for O(1) initial lookup
2. **Request/response protocol** - Use ring buffers for host→VM commands
3. **Bulk data transfer** - Protocol for large data movements
4. **Statistics page** - Performance counters and metrics
5. **Windows support** - EPROCESS structures instead of /proc
6. **Consolidate companions** - Single companion with multiple modes

## Testing

### Quick Test
```bash
# Terminal 1: Start VM
./scripts/launch_qemu_membackend.sh

# Terminal 2: Deploy and test
./deploy_camera.sh
```

### Manual Testing
```bash
# Copy to VM
scp src/companion_camera.c vm:~/
scp include/beacon_protocol.h vm:~/

# Compile in VM
ssh vm 'gcc -I. -o companion_camera companion_camera.c'

# Run companion
ssh vm './companion_camera'

# On host, scan for beacons
./test_beacon_scan
```

## Safety Notes

- Haywire is READ-ONLY except for camera focus operations
- Always verify control page before/after writes
- Bail on verification failure (indicates memory pressure)
- Never run sensitive workloads in Haywire-monitored VMs
- This tool breaks VM isolation by design

## Architecture Strengths

1. **Resilient** - Beacons found wherever malloc() places them
2. **Cooperative** - VM voluntarily shares information
3. **Structured** - Shared protocol ensures compatibility
4. **Efficient** - O(1) access after initial discovery
5. **Safe** - Read-only observation, careful write protocol