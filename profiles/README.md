# Kernel Profiles

This directory contains kernel structure offset profiles for different Linux kernels. Each profile is a JSON file that describes the memory layout of kernel data structures.

## Why Do We Need Profiles?

Haywire needs to know the exact byte offsets of fields within kernel structures like `task_struct`, `mm_struct`, and `vm_area_struct`. These offsets change between kernel versions, so we maintain profiles for each kernel we want to support.

## Supported VM Platforms

These profiles work with:
- **QEMU/KVM** - memory-backend-file or memory snapshots
- **VMware** - .vmem memory files
- **VirtualBox** - .sav snapshot files
- **Hyper-V** - .bin memory dumps
- Any platform that exposes guest physical memory

## Creating a New Profile

### Method 1: VM Console (No SSH Required)

1. **Boot your VM and access the console** (VMware console, QEMU -nographic, etc.)

2. **Login** (use whatever credentials you set up)

3. **Install pahole if needed:**
   ```bash
   sudo apt-get install dwarves
   ```

4. **Run these three commands:**
   ```bash
   pahole -C task_struct /sys/kernel/btf/vmlinux > /tmp/profile.txt
   pahole -C mm_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
   pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
   ```

5. **Copy the output** (VMware has clipboard integration, or type it manually if small)

6. **On your host, run:**
   ```bash
   cat /tmp/profile.txt | python3 scripts/create_kernel_profile.py > profiles/my-kernel.json
   ```

### Method 2: VMware Shared Folder (Easiest!)

1. **Enable VMware shared folders** in VM settings

2. **Inside VM:**
   ```bash
   sudo apt-get install dwarves
   pahole -C task_struct /sys/kernel/btf/vmlinux > /mnt/hgfs/shared/profile.txt
   pahole -C mm_struct /sys/kernel/btf/vmlinux >> /mnt/hgfs/shared/profile.txt
   pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /mnt/hgfs/shared/profile.txt
   ```

3. **On host:**
   ```bash
   python3 scripts/create_kernel_profile.py < /path/to/shared/profile.txt > profiles/my-kernel.json
   ```

### Method 3: Interactive (Paste Output)

```bash
python3 scripts/create_kernel_profile.py
# Paste the pahole output
# Press Ctrl-D when done
# JSON is written to stdout
```

## Profile Format

```json
{
  "profile": {
    "name": "Ubuntu 6.14.0-34 Generic ARM64",
    "kernel_version": "6.14.0-34-generic",
    "architecture": "aarch64",
    "verified": true
  },
  "offsets": {
    "task_struct": {
      "fields": {
        "pid": {"offset": 888, "size": 4, "type": "pid_t"},
        "comm": {"offset": 1144, "size": 16, "type": "char[16]"},
        ...
      }
    },
    "mm_struct": { ... },
    "vm_area_struct": { ... }
  }
}
```

## Using Profiles in Code

```cpp
#include "kernel_profile_loader.h"

KernelProfile profile;
if (KernelProfileLoader::LoadProfile("profiles/ubuntu-6.14.0-34-arm64.json", profile)) {
    // Use offsets
    uint32_t pid = *(uint32_t*)(task + profile.task_pid);
}
```

## Available Profiles

- `ubuntu-6.14.0-34-arm64.json` - Ubuntu 24.04 LTS, kernel 6.14.0-34, ARM64

## Contributing Profiles

If you create a profile for a kernel not yet in this directory, please consider contributing it! Just create a pull request with the new JSON file.

## BTF Requirements

Modern Linux kernels (5.2+) include BTF (BPF Type Format) data at `/sys/kernel/btf/vmlinux`. This is enabled by default in Ubuntu, Debian, Fedora, and most major distributions.

To check if your kernel has BTF:
```bash
ls -lh /sys/kernel/btf/vmlinux
# Should show a ~7MB file
```

If missing, you need a kernel compiled with `CONFIG_DEBUG_INFO_BTF=y`.

## No SSH Required!

All methods work through **VM console access only** - no network setup, no SSH certificates, no key management. Just boot the VM, login at the console, and run pahole.
