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

## TODO/Future Work

1. Implement bulk process list extraction
2. Add VMA/memory range walking  
3. Create shared memory return channel
4. Add Windows EPROCESS support
5. Consider stealth memory-backend-file approach

## Debugging Tips

- Use `fprintf(stderr, ...)` in QEMU code for debugging
- Check `/tmp/kernel_dump.txt` for QMP dump output
- Memory file is at `/tmp/haywire-vm-mem` (if configured)
- QMP port is usually 4445, monitor port 4444

## Contact/Issues

File issues in the GitHub repository. This is a research project - use at your own risk!