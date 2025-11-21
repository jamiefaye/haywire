# QEMU x86_64 Build Plan for Windows VM Support

## Current Status

**Date:** November 4, 2025

**Goal:** Build QEMU x86_64 with kernel-memory-expose patch to access Windows/Linux VM kernel structures beyond 4GB

## Step 1: Initialize Submodules ✅ (In Progress)

```bash
wsl
cd /mnt/c/Users/jamie/haywire/qemu-mods/qemu-src
git submodule update --init --recursive
```

This downloads QEMU firmware ROMs and dependencies. **Currently running...**

## Step 2: Build Vanilla QEMU x86_64

Once submodules finish:

```bash
cd /mnt/c/Users/jamie/haywire/qemu-mods
./build-qemu-x86_64.sh
```

This builds:
- Target: x86_64-softmmu (can run Windows/Linux x86_64 VMs)
- Output: `qemu-src/build-x86/qemu-system-x86_64.exe`
- Time: ~5-10 minutes

**Test it works:**
```bash
./qemu-src/build-x86/qemu-system-x86_64.exe --version
```

Should show: `QEMU emulator version 9.1.0`

## Step 3: Apply Kernel Memory Patch

You already have `kernel-memory-expose.patch` but it's configured for ARM64 at 5GB.

For x86_64, we need to change:
```c
// OLD (ARM64):
#define KERNEL_MEM_START 0x140000000ULL  /* 5GB */

// NEW (x86_64):
#define KERNEL_MEM_START 0x100000000ULL  /* 4GB */
```

**Create x86_64 version of the patch:**

```bash
cd /mnt/c/Users/jamie/haywire/qemu-mods

# Copy the ARM64 patch
cp kernel-memory-expose.patch kernel-memory-expose-x86.patch

# Edit the new patch - change KERNEL_MEM_START from 0x140000000 to 0x100000000
sed -i 's/0x140000000ULL/0x100000000ULL/g' kernel-memory-expose-x86.patch

# Apply the patch
cd qemu-src
git apply ../kernel-memory-expose-x86.patch
```

## Step 4: Rebuild with Patch

```bash
cd /mnt/c/Users/jamie/haywire/qemu-mods/qemu-src/build-x86
make -j$(nproc)
```

This rebuilds only the modified files (~2 minutes).

## Step 5: Test with Linux x86_64 VM

**Launch script will need:**

```bash
#!/bin/bash
MEMFILE_RAM="/tmp/haywire-vm-mem"       # Guest RAM (0-8GB)
MEMFILE_KERNEL="/tmp/haywire-kernel-mem" # Kernel structures (4GB+)
MEMSIZE="8G"

./qemu-src/build-x86/qemu-system-x86_64.exe \
    -M pc \
    -m $MEMSIZE \
    -object memory-backend-file,id=mem,size=$MEMSIZE,mem-path=$MEMFILE_RAM,share=on,prealloc=on \
    -numa node,memdev=mem \
    -smp 4 \
    -drive file=linux_x86_64.qcow2,format=qcow2 \
    -qmp tcp:localhost:4445,server=on,wait=off \
    -monitor stdio
```

**After starting:**
```bash
# Check both files exist
ls -lh /tmp/haywire-vm-mem         # Should be 8GB
ls -lh /tmp/haywire-kernel-mem     # Should be 4GB (kernel window)
```

**Test PGD access:**
- Use QMP to find PGD addresses (like before)
- If PGD is at 4.2GB (0x10800000), it's in kernel-mem file
- Offset in file: 0x10800000 - 0x100000000 = 0x800000 (8MB into kernel-mem file)

## Step 6: Test with Windows x86_64 VM

Same as Linux, but with Windows qcow2 image.

Windows kernel also allocates structures beyond guest RAM, so this patch will expose them.

## Architecture

```
QEMU x86_64 VM with 8GB RAM
├─ Guest RAM: 0x0 - 0x200000000 (8GB)
│  └─ Exposed via: /tmp/haywire-vm-mem (memory-backend-file)
│
└─ Kernel Memory: 0x100000000 - 0x200000000 (4GB window)
   └─ Exposed via: /tmp/haywire-kernel-mem (our patch)
      ├─ Linux PGDs at ~0x108000000 (4.1GB)
      ├─ Windows PGDs at ~0x10XXXXXXX
      └─ Other kernel structures
```

## Key Differences from VirtualBox Approach

| Aspect | QEMU Patch | VirtualBox Patch |
|--------|------------|------------------|
| **Complexity** | Modify 1 file | Modify 2 files |
| **Build time** | 10 mins | 2-4 hours (failed) |
| **Build success** | ✅ Works | ❌ Stuck on "Invalid number" |
| **Platform** | Cross-platform | Windows-specific nightmare |
| **Maintenance** | Clean codebase | kBuild hell |

## Files Created

- `build-qemu-x86_64.sh` - Build script for x86_64
- `kernel-memory-expose-x86.patch` - x86_64 version of patch (to be created)
- `qemu-src/build-x86/` - Build directory
- `qemu-src/build-x86/qemu-system-x86_64.exe` - Final binary

## Next Session Quick Start

If submodules are done cloning:

```bash
wsl
cd /mnt/c/Users/jamie/haywire/qemu-mods
./build-qemu-x86_64.sh
```

Wait ~10 minutes, then apply patch and test!

---

**Status:** Waiting for git submodules to finish cloning (Step 1)
**ETA:** Ready to build in ~5-10 minutes
