# CLAUDE.md - AI Assistant Guide for Haywire Project

## Project Overview

Haywire is a VM memory introspection tool that bypasses QEMU's memory isolation to inspect kernel structures and process memory without guest cooperation.

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

### Core Implementation (C++)
- `walk_process_list.cpp` - Main process walking implementation
- `find_processes_qmp.cpp` - QMP-based process discovery
- `include/` - Header files
- `src/` - Source files

### Test Scripts (Python)
- All `.py` files in root are test/research scripts
- Not part of production Haywire
- Used for prototyping and investigation

### QEMU Modifications
- `qemu-mods/` - Modified QEMU source
- `qemu-mods/qemu-src/qapi/misc.json` - QMP command definitions
- `qemu-mods/qemu-src/target/arm/arm-qmp-cmds.c` - Implementation

### Documentation
- `docs/qemu_memory_introspection.md` - Technical architecture
- `docs/build_qemu.md` - Building modified QEMU
- `docs/kernel_structs.md` - Kernel structure layouts

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

### Why Not Use memory-backend-file?
- QEMU intentionally hides kernel structures
- Security feature prevents kernel inspection
- Must use QMP bypass for access

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

## Recent Progress (September 2, 2025)

### Beacon-Based Shared Memory Protocol
- Transitioned from external memory introspection to cooperative beacon approach
- Each 4KB page has 64-byte beacon header with magic numbers (0x3142FACE, 0xCAFEBABE)
- Implemented bidirectional page tables for O(1) beacon lookups
- Protocol v4 adds beacon classes for typed memory regions

### Beacon Classes (Protocol v4)
```
Pages    0-15:   INDEX (16 pages) - Discovery and master index
Pages   16-31:   REQUEST_RING (16 pages) - Request circular buffer
Pages   32-47:   RESPONSE_RING (16 pages) - Response buffer  
Pages   48-63:   STATISTICS (16 pages) - Performance counters
Pages   64-1087: BULK_DATA (1024 pages) - Large transfers
Pages 1088-2047: RESERVED (960 pages) - Future expansion
```

### SSH Setup
- Primary user: `ubuntu` (passwordless via SSH key)
- Emergency user: `jff` with password `p`
- Host alias: `vm` (localhost:2222)
- See `docs/vm_setup_guide.md` for complete setup

## TODO/Future Work

1. Fix beacon class enum values between handler and scanner
2. Implement request/response protocol using rings
3. Add master index for O(1) discovery
4. Create bulk transfer protocol for large data
5. Add Windows EPROCESS support

## Debugging Tips

- Use `fprintf(stderr, ...)` in QEMU code for debugging
- Check `/tmp/kernel_dump.txt` for QMP dump output
- Memory file is at `/tmp/haywire-vm-mem` (if configured)
- QMP port is usually 4445, monitor port 4444

## Contact/Issues

File issues in the GitHub repository. This is a research project - use at your own risk!