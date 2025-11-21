# VirtualBox Memory Bridge Design

**Date**: November 3, 2025
**Goal**: Expose VirtualBox guest memory as a memory-mapped file for Haywire, without recompiling VirtualBox

## Executive Summary

Instead of patching VirtualBox, we'll create a **memory bridge daemon** that uses VirtualBox's COM API to continuously sync guest memory to a mmap-able file.

## Key Discovery

VirtualBox COM API provides:
- `IMachineDebugger::readPhysicalMemory(address, size)` - Read guest physical memory
- Available via COM/XPCOM without VirtualBox modifications
- Works on running VMs with debugger enabled

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ VirtualBox VM (Running)                                     │
│  ├─ Guest RAM (4GB)                                         │
│  └─ Kernel structures at various physical addresses        │
└────────────────┬────────────────────────────────────────────┘
                 │ COM API
                 │ IMachineDebugger::readPhysicalMemory()
                 ▼
┌─────────────────────────────────────────────────────────────┐
│ haywire-vbox-bridge (Daemon Process)                        │
│  ├─ Polls dirty pages periodically                          │
│  ├─ Reads via COM API                                       │
│  └─ Writes to shared memory file                            │
└────────────────┬────────────────────────────────────────────┘
                 │ Writes
                 ▼
         /tmp/haywire-vbox-mem
         (mmap'd file, MAP_SHARED)
                 ▲
                 │ mmap(MAP_SHARED)
┌────────────────┴────────────────────────────────────────────┐
│ Haywire (C++ Application)                                   │
│  ├─ MemoryFileReader                                        │
│  ├─ Kernel Discovery                                        │
│  └─ Memory Visualization                                    │
└─────────────────────────────────────────────────────────────┘
```

## Implementation Approach

### Option A: Simple Polling (Initial Proof of Concept)

**Advantages:**
- Simplest implementation (~200 lines)
- No VirtualBox modifications needed
- Works immediately

**Disadvantages:**
- Higher CPU usage (constant polling)
- Latency between guest writes and Haywire sees updates

```python
# Pseudocode
while True:
    for chunk_index in range(num_chunks):
        addr = chunk_index * CHUNK_SIZE
        data = machineDebugger.readPhysicalMemory(addr, CHUNK_SIZE)
        memfile.write_at(chunk_index * CHUNK_SIZE, data)
    sleep(0.1)  # 10 Hz refresh
```

### Option B: Dirty Page Tracking (Optimized)

**Advantages:**
- Lower CPU usage (only read changed pages)
- Faster updates for changed regions

**Disadvantages:**
- More complex implementation
- Still has polling overhead

```python
# Pseudocode
dirty_bitmap = get_dirty_pages_since_last_scan()
for page_index in dirty_bitmap:
    addr = page_index * PAGE_SIZE
    data = machineDebugger.readPhysicalMemory(addr, PAGE_SIZE)
    memfile.write_at(page_index * PAGE_SIZE, data)
```

### Option C: VirtualBox Event Hooks (Advanced)

**Advantages:**
- Zero polling overhead
- Immediate updates

**Disadvantages:**
- Requires finding undocumented event hooks
- May not exist in COM API

## Testing Plan

### Phase 1: Verify readPhysicalMemory Works

```python
# test_vbox_memory_api.py
import sys
from vboxapi import VirtualBoxManager

mgr = VirtualBoxManager(None, None)
vbox = mgr.vbox
session = mgr.mgr.getSessionObject(vbox)

# Attach to running VM
machine = vbox.findMachine("ubuntu-test")
machine.lockMachine(session, 1)  # LockType_Shared

console = session.console
debugger = console.debugger

# Test: Read first 4KB of guest RAM
try:
    data = debugger.readPhysicalMemory(0x40000000, 4096)
    print(f"Success! Read {len(data)} bytes")
    print(f"First 64 bytes: {data[:64].hex()}")
except Exception as e:
    print(f"Error: {e}")
    print("readPhysicalMemory may not be implemented")

session.unlockMachine()
```

**Expected Results:**
- If implemented: Returns 4096 bytes of guest memory
- If not implemented: Exception with "not implemented" message

### Phase 2: Build Memory Bridge Daemon

**File:** `scripts/haywire-vbox-bridge.py`

```python
#!/usr/bin/env python3
"""
VirtualBox Memory Bridge Daemon

Continuously syncs guest physical memory from VirtualBox VM
to a memory-mapped file that Haywire can read.
"""

import sys
import mmap
import time
import argparse
from vboxapi import VirtualBoxManager

RAM_BASE = 0x40000000  # ARM64, adjust for x86_64
RAM_SIZE = 4 * 1024 * 1024 * 1024  # 4GB
CHUNK_SIZE = 64 * 1024  # 64KB chunks
MEMORY_FILE = "/tmp/haywire-vbox-mem"

def main():
    parser = argparse.ArgumentParser(description="VirtualBox Memory Bridge")
    parser.add_argument("vm_name", help="Name of running VM")
    parser.add_argument("--refresh-hz", type=int, default=10, help="Refresh rate in Hz")
    args = parser.parse_args()

    # Connect to VirtualBox
    mgr = VirtualBoxManager(None, None)
    vbox = mgr.vbox
    session = mgr.mgr.getSessionObject(vbox)

    # Find and attach to VM
    machine = vbox.findMachine(args.vm_name)
    machine.lockMachine(session, 1)  # LockType_Shared
    console = session.console
    debugger = console.debugger

    print(f"Connected to VM: {args.vm_name}")

    # Create memory-mapped file
    with open(MEMORY_FILE, "wb") as f:
        f.write(b'\x00' * RAM_SIZE)

    fd = open(MEMORY_FILE, "r+b")
    mem = mmap.mmap(fd.fileno(), RAM_SIZE, access=mmap.ACCESS_WRITE)

    print(f"Created memory file: {MEMORY_FILE} ({RAM_SIZE} bytes)")
    print(f"Refresh rate: {args.refresh_hz} Hz")
    print("Press Ctrl+C to stop")

    num_chunks = RAM_SIZE // CHUNK_SIZE
    interval = 1.0 / args.refresh_hz

    try:
        while True:
            start_time = time.time()

            # Read all memory chunks
            for i in range(num_chunks):
                addr = RAM_BASE + i * CHUNK_SIZE
                try:
                    data = debugger.readPhysicalMemory(addr, CHUNK_SIZE)
                    mem[i * CHUNK_SIZE:(i + 1) * CHUNK_SIZE] = data
                except Exception as e:
                    # Skip inaccessible regions
                    pass

            # Rate limiting
            elapsed = time.time() - start_time
            sleep_time = max(0, interval - elapsed)
            time.sleep(sleep_time)

            # Status update
            if int(time.time()) % 10 == 0:
                print(f"Synced {num_chunks} chunks in {elapsed:.3f}s")

    except KeyboardInterrupt:
        print("\nShutting down...")

    finally:
        mem.close()
        fd.close()
        session.unlockMachine()

if __name__ == "__main__":
    main()
```

### Phase 3: Test with Haywire

```bash
# Terminal 1: Start VirtualBox VM (with debugging enabled)
VBoxManage modifyvm ubuntu-test --debugger on
VBoxManage startvm ubuntu-test --type headless

# Terminal 2: Start memory bridge
python3 scripts/haywire-vbox-bridge.py ubuntu-test

# Terminal 3: Run Haywire
./build/haywire --memory-file /tmp/haywire-vbox-mem --ram-base 0x40000000
```

## Performance Considerations

### Bandwidth Requirements

For 4GB RAM at 10 Hz full scan:
- 4GB × 10 = 40 GB/s (unrealistic!)

**Solution:** Don't scan everything every frame

**Optimized scanning:**
- Viewport region: 20 Hz (fast updates for visible area)
- Heat map region: 4 Hz (moderate updates for overview)
- Rest: 0.1 Hz (background, catch changes)

This reduces bandwidth to ~500 MB/s (achievable)

### readPhysicalMemory Performance

Need to test actual throughput:
- Per-call overhead (COM invocation)
- Optimal chunk size (4KB? 64KB? 1MB?)
- Thread safety (can we parallelize?)

## Comparison: Bridge vs Patch Approach

| Aspect | Memory Bridge (COM) | Secret Range Patch |
|--------|-------------------|-------------------|
| **Implementation** | ~300 lines Python | ~150 lines C in VBox source |
| **Compilation** | None needed | Must compile VirtualBox |
| **Latency** | 10-100ms (polling) | 0ms (direct mmap) |
| **CPU Usage** | 5-15% (polling) | <1% (direct access) |
| **Maintenance** | Update Python script | Rebase patch on VBox updates |
| **Setup Time** | 10 minutes | 2-3 days (build + test) |

## Decision Matrix

**Use Memory Bridge If:**
- ✅ Need working solution quickly (hours, not days)
- ✅ Don't want to compile VirtualBox
- ✅ Can tolerate 10-100ms latency
- ✅ Python environment available

**Use Secret Range Patch If:**
- ✅ Need zero-latency (real-time visualization)
- ✅ Okay with maintaining custom VirtualBox fork
- ✅ Have 2-3 days for build/test
- ✅ Want optimal performance

## Immediate Next Steps

1. **Test if readPhysicalMemory is implemented** (5 minutes)
   ```bash
   python3 test_vbox_memory_api.py
   ```

2. **If YES → Build memory bridge** (2-3 hours)
   - Implement polling daemon
   - Test with Haywire
   - Optimize chunk sizes and refresh rates

3. **If NO → Explore alternatives:**
   - Check if newer VirtualBox versions implemented it
   - Look for alternative COM API methods
   - Consider the secret range patch approach

## Status

- [x] Researched VirtualBox COM API
- [x] Identified readPhysicalMemory method
- [x] Designed memory bridge architecture
- [ ] Test if readPhysicalMemory is actually implemented
- [ ] Implement proof-of-concept bridge
- [ ] Integrate with Haywire

## References

- VirtualBox SDK: https://www.virtualbox.org/sdkref/
- IMachineDebugger: https://www.virtualbox.org/sdkref/interface_i_machine_debugger.html
- Ticket #10222: https://www.virtualbox.org/ticket/10222 (readPhysicalMemory feature request)
- VirtualBox Python API: https://www.virtualbox.org/manual/ch11.html#vboxshell
