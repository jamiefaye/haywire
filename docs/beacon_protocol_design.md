# Haywire Beacon Protocol Design v2.0

## Overview

The beacon protocol enables a companion process running inside a VM guest to share system information with Haywire running on the host, without requiring guest cooperation or network communication. Data is shared through memory pages that are discoverable via QEMU's memory-backend-file.

## Architecture Components

### 1. Master Beacon Block
- **Location**: Fixed at start of beacon array, easily discoverable
- **Purpose**: Central directory and communication hub
- **Contains**:
  - PTE mappings for all other blocks
  - Camera request/response protocol
  - Block validation status
  - Generation counters

### 2. PID Discovery Block  
- **Purpose**: Rapid enumeration of all PIDs in system
- **Update Rate**: Every cycle (fast, cheap scan)
- **Format**: Simple circular buffer of PID numbers
- **Use Case**: Detect new processes and process termination

### 3. Round-Robin Block
- **Purpose**: Baseline data for all processes
- **Contains**: Process info + memory sections (NO pagemaps)
- **Update Strategy**: One PID per cycle, rotating through all
- **Format**: Variable-size streaming with tear detection
- **Use Case**: Instant approximate data when switching focus

### 4. Camera Blocks (Focus Pools)
- **Count**: 2 pre-allocated, can recycle as needed
- **Purpose**: Detailed monitoring of specific PIDs
- **Contains**: Process info + sections + pagemap (RLE compressed)
- **Update Rate**: Every cycle for focused PIDs
- **Isolation**: Each camera independent, no tear interference

## Data Structures

### Standard Beacon Page Header (32 bytes)
```c
typedef struct {
    uint32_t magic;          // 0x3142FACE
    uint32_t block_type;     // MASTER|PIDS|RR|CAMERA
    uint32_t block_id;       // Which camera, etc
    uint32_t page_index;     // Page N within block
    uint32_t generation;     // Version counter
    uint32_t data_size;      // Valid data bytes
    uint64_t timestamp;      // When written
    uint32_t checksum;       // Page validation
} BeaconPageHeader;
```

### Master Beacon Structure
```c
typedef struct {
    BeaconPageHeader header;
    
    // Block directory with PTEs
    struct {
        uint64_t pte;        // Physical address
        uint32_t page_count; 
        uint32_t status;     // VALIDATING|ACTIVE|INVALID
        uint32_t last_valid_gen;
    } blocks[32];
    
    // Camera communication
    struct {
        uint32_t request_pid;  // Haywire → Companion
        uint32_t priority;
    } camera_requests[16];
    
    struct {
        uint32_t pid;          // Companion → Haywire  
        uint32_t status;
        uint64_t data_pte;
        uint32_t generation;
    } camera_status[16];
} MasterBeacon;
```

## Key Design Decisions

### 1. Tear-Resistant Design
- Every page self-contained with magic/checksum
- Generation numbers for version tracking
- Fixed-offset structures where possible
- Master beacon validates before publishing PTEs

### 2. Space Efficiency
- Round-robin uses variable-size entries
- RLE compression for sparse pagemaps
- No pagemap in round-robin (too heavy)
- Windowing for pathological processes (>10GB)

### 3. Performance Optimization
- PTE hints for direct physical access
- No searching after initial discovery
- Sequential writes for better cache behavior
- Bulk reads of /proc filesystem

### 4. Scalability
- Start with 2 cameras, recycle as needed
- Windowed mode for huge processes
- Fixed chunk RLE for predictable parsing
- Master beacon enables dynamic allocation

## Companion Implementation Flow

### Initialization
1. Allocate beacon pages (8MB typical)
2. Write discovery pattern in master beacon
3. Get PTEs of own pages for hints
4. Publish master beacon as ready

### Main Loop
```python
while running:
    # Always update
    update_pid_discovery()      # ~0.4ms
    update_one_rr_pid()         # ~2ms
    
    # Check for camera requests
    check_camera_requests()
    
    # Update active cameras
    for camera in active_cameras:
        update_camera_full(camera)  # ~10ms per camera
    
    # Validate and publish
    validate_all_blocks()
    update_master_beacon()
    
    sleep(10ms + jitter)
```

### Camera Allocation Protocol
1. Haywire writes request_pid in master beacon
2. Companion allocates camera pages
3. Companion fills camera with process data
4. Companion writes PTEs to master beacon
5. Haywire reads camera via direct PTE access

## Haywire Implementation Flow

### Discovery
1. Scan for master beacon (repeated magic pattern)
2. Read PTE hints from master
3. Direct access all blocks via PTEs
4. Fallback to full scan if validation fails

### Monitoring
```python
# Background thread
continuously:
    read_pid_discovery()  # New/dead processes
    read_round_robin()    # Baseline data
    
# Focus management
on_user_click(pid):
    if has_round_robin_data(pid):
        display_immediately()  # Instant gratification
    
    request_camera(pid)
    wait_for_camera_ready()
    display_detailed_view()
```

## Memory Layout Example

```
Pages 0:      Master beacon (4KB)
Pages 1-10:   PID discovery (40KB)  
Pages 11-50:  Round-robin (160KB)
Pages 51-114: Camera 0 (256KB)
Pages 115-178: Camera 1 (256KB)
Total: ~720KB active usage (of 8MB allocated)
```

## Pagemap RLE Encoding

### Fixed Chunk Format
```c
typedef struct {
    uint32_t magic;         // 0xRLE chunk start
    uint32_t base_page;     // Starting page number
    uint32_t num_runs;      // Runs in this chunk
    
    struct {
        uint32_t run_type;  // EMPTY|PRESENT|SWAPPED
        uint32_t run_length;
        uint64_t value;     // For non-empty
    } runs[];
    
    uint32_t magic_end;     // Validation
} RLEChunk;
```

### Compression Ratios
- Sparse allocation (2 pages of 256): 42x compression
- Typical process: 10-20x compression  
- Dense allocation: 1-2x (minimal overhead)

## Error Handling

### Companion
- Abort on beacon corruption
- Clear status on PID death
- Recycle cameras on memory pressure
- Validate before publishing to master

### Haywire  
- Fallback to full scan if hints fail
- Skip corrupted blocks
- Retry camera requests
- Display stale data with indicator

## Performance Characteristics

### Update Rates
- PID discovery: 100Hz (every 10ms)
- Round-robin: ~0.5Hz per PID (200 PIDs = 4s full cycle)
- Camera: 100Hz (every 10ms for focused PIDs)

### Latencies
- Focus switch with RR data: <1ms
- New camera allocation: ~20ms
- Full pagemap update: ~10ms (2GB process)
- Windowed update: ~5ms per window

### Memory Usage
- Companion: 8MB beacon array
- Per-process overhead: ~200KB (RR) or 2-4MB (camera)
- Haywire: Negligible (reads via mmap)

## Future Extensions

### Considered but Deferred
- Command buffer for file inspection
- Network socket monitoring
- Thread stack traces
- Kernel structure walking

### Planned Improvements
- Windows companion (using Windows APIs)
- Multi-VM coordination
- Historical data recording
- Performance profiling integration

## Security Considerations

- No encryption (assumes trusted host)
- No authentication (physical memory access)
- Read-only monitoring (no guest modification)
- Companion should run unprivileged when possible

## Compatibility

### Linux Guest
- Primary target, fully supported
- Uses /proc filesystem
- Tested on kernel 5.x, 6.x

### macOS Guest  
- Prototype exists using sysctl/mach APIs
- Limited by entitlements/code signing
- Memory region data less detailed

### Windows Guest
- Planned using Windows Tool Help APIs
- NtQuerySystemInformation for details
- EPROCESS walking possible

## Summary

This beacon protocol provides efficient, tear-resistant, scalable monitoring of VM guests. The master beacon architecture enables dynamic resource allocation while PTE hints eliminate searching overhead. The combination of round-robin baseline data and focused cameras provides both breadth and depth of visibility with minimal overhead.