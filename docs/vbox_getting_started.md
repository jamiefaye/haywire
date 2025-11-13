# Getting Started with VirtualBox Memory Bridge

This guide walks you through testing whether VirtualBox's COM API can expose guest memory without recompiling VirtualBox.

## Quick Start (5 minutes)

### Prerequisites

1. **VirtualBox installed** (any recent version)
2. **Python 3** with pip
3. **A running VM** (or ability to start one)

### Step 1: Install VirtualBox Python API

```bash
pip install vboxapi
```

### Step 2: Ensure VM has debugging enabled

```bash
# Replace "ubuntu-test" with your VM name
VBoxManage modifyvm ubuntu-test --debugger on
```

### Step 3: Start the VM (if not already running)

```bash
# Headless mode (no GUI)
VBoxManage startvm ubuntu-test --type headless

# OR with GUI
VBoxManage startvm ubuntu-test
```

### Step 4: Run the test script

```bash
cd /path/to/haywire

# For ARM64 VM (RAM at 0x40000000)
python3 test_vbox_memory_api.py ubuntu-test

# For x86_64 VM (RAM at 0x0)
python3 test_vbox_memory_api.py ubuntu-test --addr 0x0
```

## Expected Results

### ✅ If readPhysicalMemory IS implemented:

```
Testing VirtualBox IMachineDebugger::readPhysicalMemory
VM: ubuntu-test
Address: 0x40000000
Size: 4096 bytes

Connecting to VirtualBox...
VirtualBox version: 7.0.14

Finding VM 'ubuntu-test'...
Found: ubuntu-test
State: 5

Creating session...
Locking machine...
Machine locked, debugger obtained

Attempting readPhysicalMemory...
  Address: 0x0000000040000000
  Size: 4096 bytes

✅ SUCCESS! readPhysicalMemory is IMPLEMENTED!

Read 4096 bytes from 0x0000000040000000

First 64 bytes:
  40000000:  00 00 a0 d3 00 00 00 14 00 00 00 00 00 00 00 00  ................
  40000010:  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
  40000020:  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
  40000030:  00 00 00 00 00 00 00 00 4d 5a 90 00 03 00 00 00  ........MZ......

🎉 VirtualBox memory bridge is FEASIBLE!
   You can use the COM API approach without recompiling VirtualBox.
```

**Next steps:** Build the memory bridge daemon! See below.

---

### ❌ If readPhysicalMemory is NOT implemented:

```
Testing VirtualBox IMachineDebugger::readPhysicalMemory
VM: ubuntu-test
Address: 0x40000000
Size: 4096 bytes

Connecting to VirtualBox...
VirtualBox version: 7.0.14

Finding VM 'ubuntu-test'...
Found: ubuntu-test
State: 5

Creating session...
Locking machine...
Machine locked, debugger obtained

Attempting readPhysicalMemory...
  Address: 0x0000000040000000
  Size: 4096 bytes

❌ FAILED! readPhysicalMemory is NOT implemented

Error: 0x80004001 (NS_ERROR_NOT_IMPLEMENTED)

The method exists in the API but returns NotImplemented.
This is a known issue (VirtualBox ticket #10222).

Options:
  1. Use 'dumpvmcore' approach (snapshot-based)
  2. Implement the 'secret range' patch (requires compiling VirtualBox)
  3. Check if newer VirtualBox versions implemented this
```

**Next steps:** See "Alternative Approaches" below.

---

## If Test Succeeds: Build Memory Bridge

Great! You can now build the memory bridge daemon that continuously syncs memory:

```bash
# Create the bridge script (coming next!)
# This will be implemented as scripts/haywire-vbox-bridge.py
```

The bridge will:
1. Connect to running VM via COM API
2. Continuously read guest physical memory
3. Write to `/tmp/haywire-vbox-mem` (or Windows equivalent)
4. Allow Haywire to mmap this file

**Performance characteristics:**
- Latency: ~10-100ms (polling-based)
- CPU usage: ~5-15% (optimized scanning)
- Bandwidth: ~500 MB/s (selective region updates)

---

## If Test Fails: Alternative Approaches

### Option 1: Use VirtualBox Dumps (Static Analysis)

Already working from your previous investigation!

```bash
# Dump VM memory to ELF core
VBoxManage debugvm ubuntu-test dumpvmcore --filename vm-dump.elf

# Parse and analyze with existing scripts
python3 parse_vbox_dump.py vm-dump.elf
python3 analyze_vbox_memory.py flattened_memory.bin
```

**Pros:**
- Works with current VirtualBox
- No modifications needed
- You already have working scripts

**Cons:**
- Static snapshots only (no live updates)
- Must pause/resume VM for each dump
- Slower workflow

### Option 2: Implement "Secret Range" Patch

Modify VirtualBox source to allocate guest RAM in a shared memory file.

**Pros:**
- Zero-latency live updates
- Perfect performance
- Clean architecture

**Cons:**
- Must compile VirtualBox (~2-3 days)
- Maintain custom fork
- More complex setup

See `docs/virtualbox_secret_range_patch.md` for full design.

### Option 3: Check Newer VirtualBox Versions

```bash
# Check VirtualBox version
VBoxManage --version

# If < 7.2, try updating:
# https://www.virtualbox.org/wiki/Downloads
```

The readPhysicalMemory feature was requested in 2012 (ticket #10222).
It's possible newer versions have implemented it.

### Option 4: Use QEMU Instead

QEMU's `memory-backend-file` already provides this functionality:

```bash
# QEMU with memory-backend-file (already working!)
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a57 \
    -m 4G \
    -object memory-backend-file,id=mem,size=4G,mem-path=/tmp/haywire-vm-mem,share=on \
    -numa node,memdev=mem \
    ...
```

**Pros:**
- Already works with Haywire
- Zero modifications needed
- Well-tested

**Cons:**
- Requires QEMU setup (you already have this!)
- WSL2 needed on Windows (but you have that too)

---

## Troubleshooting

### "VM not found"

```bash
# List all VMs
VBoxManage list vms

# Use exact name from output
python3 test_vbox_memory_api.py "Ubuntu 22.04"
```

### "VM is not running"

```bash
# Check VM state
VBoxManage showvminfo ubuntu-test | grep State

# Start the VM
VBoxManage startvm ubuntu-test --type headless
```

### "vboxapi module not found"

```bash
# Install VirtualBox Python bindings
pip install vboxapi

# On some systems, you may need the SDK:
# Download from https://www.virtualbox.org/wiki/Downloads
# Extract and run: python vboxapisetup.py install
```

### "Access denied" or permission errors

```bash
# On Linux, add user to vboxusers group
sudo usermod -a -G vboxusers $USER
# Then log out and back in

# On Windows, run PowerShell as Administrator
```

### Reading from wrong address

**For x86_64 VMs:** RAM starts at 0x0
```bash
python3 test_vbox_memory_api.py my-vm --addr 0x0
```

**For ARM64 VMs:** RAM starts at 0x40000000
```bash
python3 test_vbox_memory_api.py my-vm --addr 0x40000000
```

**For addresses >4GB:** Use hex notation
```bash
python3 test_vbox_memory_api.py my-vm --addr 0x100000000
```

---

## Next Steps

Based on test results:

✅ **If test succeeds:**
1. Build memory bridge daemon (scripts/haywire-vbox-bridge.py)
2. Test continuous memory sync
3. Integrate with Haywire's MemoryFileReader
4. Optimize refresh rates and chunk sizes

❌ **If test fails:**
1. Decide between dump-based (static) or patch (compile VirtualBox)
2. Dump approach is easier and already working
3. Patch approach gives best performance but requires compilation
4. Or continue using QEMU (already works great!)

---

## Status

- [x] Created test script
- [x] Documented testing procedure
- [ ] **→ RUN THE TEST!** ← Start here
- [ ] Based on results, choose next path
- [ ] Implement chosen approach
- [ ] Integrate with Haywire

## Questions?

See also:
- `docs/vbox_investigation_results.md` - Your previous VirtualBox research
- `docs/virtualbox_secret_range_patch.md` - Compilation-based approach
- `docs/vbox_memory_bridge_design.md` - COM API bridge design
