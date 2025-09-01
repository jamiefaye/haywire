# QEMU Memory Introspection Architecture

## Overview

This document describes the architecture for bypassing QEMU's memory isolation to enable kernel-level introspection of guest VMs without requiring guest agents.

## Problem Statement

QEMU implements a security mechanism that separates guest RAM (accessible via memory-backend-file) from kernel structures (only accessible via internal QEMU functions). This prevents host-level inspection of critical kernel data structures needed for process introspection.

### Key Findings

1. **Memory Isolation Mechanism**: QEMU places kernel structures (page tables, task_struct, etc.) in memory regions beyond guest RAM boundaries
2. **highmem=off doesn't help**: Even with highmem=off, kernel structures remain inaccessible via memory-backend-file
3. **Address ranges**:
   - With highmem=on: Kernel structures at ~6.8GB (0x1b4dbf000)
   - With highmem=off: Kernel structures at ~2.77GB (0xb11bf000)
   - Both beyond the memory-backend-file scope

## Solution Architecture

### Core Data Structures

```c
typedef struct MemoryRange {
    uint64_t vaddr_start;
    uint64_t vaddr_end;
    uint64_t paddr_base;
    uint32_t permissions;  // R/W/X
    char name[64];         // "[heap]", "[stack]", "libc.so", etc.
} MemoryRange;

typedef struct ProcessMemoryMap {
    uint32_t pid;
    char comm[16];
    uint64_t page_table_base;  // CR3/TTBR0/SATP
    uint32_t num_ranges;
    MemoryRange ranges[256];
} ProcessMemoryMap;
```

### Required QMP Commands

1. **get-process-list**: Returns all processes with PIDs, names, and page table bases
2. **get-process-memory-map**: Returns memory ranges (VMAs) for a specific process
3. **translate-range**: Bulk VA→PA translation for a memory range

### Implementation Approach

#### Phase 1: Linux on ARM64/x86-64
- Use existing kernel structure knowledge
- Walk task_struct linked list via QMP
- Extract TTBR0/CR3 for each process
- Use cpu_get_phys_page_debug() for address translation

#### Phase 2: Windows Support
- Detect Windows via kernel signatures
- Walk EPROCESS list
- Extract DirectoryTableBase (CR3)
- Parse VAD tree for memory ranges

#### Phase 3: macOS Support (Optional)
- More complex due to XNU kernel
- Limited by macOS hypervisor framework

## Security Considerations

### WARNING
**Never run sensitive workloads in Haywire-monitored VMs:**
- SSH keys, GPG keys, certificates
- Password managers, credential stores
- Cryptocurrency wallets
- Banking applications
- Corporate secrets

### What Gets Exposed
- Kernel keyrings and credential caches
- Swap encryption keys
- KASLR secrets
- Process memory from all applications
- Network encryption keys (IPsec, WireGuard)

## Implementation Status

### Completed
- [x] QMP VA→PA translation (query-va2pa)
- [x] Kernel info extraction (query-kernel-info)
- [x] Basic process dumping (dump-kernel-processes)
- [x] Defeated memory isolation via cpu_physical_memory_read()

### In Progress
- [ ] Bulk process list extraction
- [ ] VMA/memory range walking
- [ ] Batch VA→PA translation

### Future Work
- [ ] Windows EPROCESS walking
- [ ] Shared memory return channel for performance
- [ ] Stealth memory-backend-file for kernel structures
- [ ] Symbol resolution helpers

## Performance Optimizations

### Current Performance
- Process list walk: ~300ms via QMP
- Single VA→PA translation: ~5ms

### Target Performance
- Bulk process list: <20ms
- Memory range translation: <50ms for 1GB range
- Use shared memory to avoid JSON serialization overhead

## Testing

All test scripts are in Python for rapid prototyping:
- `test_highmem_off.py` - Tests highmem=off configuration
- `investigate_hiding.py` - Analyzes memory hiding mechanism
- `walk_kernel_pagetables.py` - Page table walking tests

Production code in C++ (`walk_process_list.cpp`).

## Building Modified QEMU

See `docs/build_qemu.md` for instructions on building QEMU with our introspection modifications.

## Files Modified in QEMU

1. `qapi/misc.json` - Added QMP command definitions
2. `target/arm/arm-qmp-cmds.c` - Implemented introspection commands
3. `backends/hostmem-file.c` - (Experimental) 2GB hack for extended visibility

## Usage Example

```python
# Get process list
processes = qmp_command("get-process-list", {"cpu-index": 0})

# Find target process
for p in processes:
    if p['name'] == 'nginx':
        # Get memory map
        memory_map = qmp_command("get-process-memory-map", {
            "cpu-index": 0,
            "pid": p['pid']
        })
        
        # Translate addresses for heap
        for range in memory_map['ranges']:
            if '[heap]' in range['name']:
                translations = qmp_command("translate-range", {
                    "cpu-index": 0,
                    "page-table-base": p['page_table_base'],
                    "vaddr-start": range['start'],
                    "vaddr-end": range['end']
                })
```

## References

- ARM64 page table format documentation
- Linux task_struct layout (varies by kernel version)
- Windows EPROCESS structure
- QEMU memory API documentation