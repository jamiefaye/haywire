# Obsolete Components

This directory contains components that have been replaced by the kernel discovery system.

## Beacon Architecture (OBSOLETE)
The beacon system used specially crafted memory patterns injected by companion processes to mark memory regions. This has been replaced by direct kernel structure discovery.

### Beacon Files:
- `beacon_*.cpp/h` - Beacon encoding/decoding/reading system
- `beacon_protocol.h` - Protocol definitions for beacon markers
- `beacon_architecture_current.md` - Documentation of the beacon system
- Various test files for beacon functionality

## Companion Processes (OBSOLETE)
Companion processes ran inside the guest VM to inject beacons and provide memory mapping information. This has been replaced by kernel discovery that reads task_struct, mm_struct, and page tables directly.

### Companion Files:
- `companion_oneshot.c` - Single-shot companion that dumped process info
- `companion_triggered.c` - Triggered companion for on-demand updates
- `deploy_companion_v2.sh` - Script to deploy companion to guest VM
- `scan_companion*.py` - Python scripts for companion testing

## Why These Are Obsolete

The kernel discovery system (in `src/kernel_discovery.cpp` and `src/kernel_discovery_backend.cpp`) now:
1. Gets swapper PGD directly from QMP
2. Scans memory to find all task_struct structures
3. Extracts PGDs from mm_struct for each process
4. Walks maple trees to get memory sections
5. Builds PTE maps for VA→PA translation

This eliminates the need for:
- Guest cooperation (companion processes)
- Beacon injection and scanning
- SSH access to the guest VM

The new system is more reliable, faster, and works without any guest-side components.