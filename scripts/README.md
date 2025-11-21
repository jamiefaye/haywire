# Haywire Launch Scripts

This directory contains the canonical scripts for launching VMs with Haywire support.

## VM Launch Scripts

### Linux Guests

**`launch_linux_arm64_macos.sh`** - ARM64 Linux on macOS (Apple Silicon)
- **Guest OS**: Ubuntu 24.04 ARM64
- **Host Platform**: macOS (Apple Silicon M1/M2/M3)
- **Acceleration**: HVF (Hypervisor.framework)
- **Use Case**: Primary development platform for Haywire
- **Features**:
  - memory-backend-file at `/tmp/haywire-vm-mem`
  - QMP on port 4445
  - QEMU Monitor on port 4444
  - SSH forwarding: localhost:2222 → guest:22
  - Custom QEMU build with VA→PA translation support

**`launch_linux_x86_64_linux.sh`** - x86_64 Linux on Linux Host
- **Guest OS**: Ubuntu 24.04 x86_64
- **Host Platform**: Linux (Intel/AMD)
- **Acceleration**: KVM
- **Use Case**: Testing on native Linux systems
- **Features**:
  - memory-backend-file at `/tmp/haywire-vm-mem`
  - QMP on port 4445
  - QEMU Monitor on port 4444
  - SSH forwarding: localhost:2222 → guest:22
  - UEFI boot with OVMF

### Windows Guests

**`launch_windows_x86_64_linux.sh`** - Windows 11 on Linux Host
- **Guest OS**: Windows 11 (24H2 or later)
- **Host Platform**: Linux (Intel/AMD)
- **Acceleration**: KVM
- **Use Case**: Testing Windows kernel introspection
- **Requirements**:
  - Windows 11 ISO
  - OVMF UEFI firmware
  - swtpm for TPM 2.0 emulation
  - 8GB RAM minimum
- **Features**:
  - memory-backend-file at `/tmp/haywire-vm-mem`
  - QMP on port 4445
  - QEMU Monitor on port 4444
  - VirtIO drivers support

**`launch_windows_x86_64_macos.sh`** - Windows 11 on macOS (Apple Silicon)
- **Guest OS**: Windows 11 (x86_64)
- **Host Platform**: macOS (Apple Silicon M1/M2/M3)
- **Acceleration**: TCG (software emulation - SLOW)
- **Use Case**: Testing Windows kernel introspection on macOS
- **Requirements**:
  - QEMU (`brew install qemu`)
  - Existing Windows 11 disk image
  - OVMF UEFI firmware (included with QEMU)
  - 8GB RAM minimum
- **Features**:
  - memory-backend-file at `/tmp/haywire-vm-mem`
  - QMP on port 4445
  - QEMU Monitor on port 4444
  - RDP forwarding: localhost:3389 → guest:3389
- **Note**: Performance will be slow due to software emulation of x86_64 on ARM64

## Utility Scripts

**`extract_kernel_offsets.sh`** - Extract kernel structure offsets
- Uses `pahole` to extract offsets from kernel BTF data
- Creates kernel profile JSON files

**`get_kernel_offsets.sh`** - SSH-based kernel offset extraction
- Connects to VM via SSH to extract offsets
- Generates profile files for kernel discovery

**`sign_haywire.sh`** - macOS code signing
- Signs Haywire binary for debugging privileges
- Required for task_for_pid access on macOS

## Platform Support Matrix

| Guest OS | Host Platform | Script | Acceleration |
|----------|---------------|--------|--------------|
| Linux ARM64 | macOS (Apple Silicon) | `launch_linux_arm64_macos.sh` | HVF |
| Linux x86_64 | Linux (Intel/AMD) | `launch_linux_x86_64_linux.sh` | KVM |
| Windows 11 x86_64 | Linux (Intel/AMD) | `launch_windows_x86_64_linux.sh` | KVM |
| Windows 11 x86_64 | macOS (Apple Silicon) | `launch_windows_x86_64_macos.sh` | TCG (slow) |

## Quick Start

### macOS (Apple Silicon)
```bash
# Launch Ubuntu ARM64 VM (fast, native)
./scripts/launch_linux_arm64_macos.sh

# Or launch Windows 11 x86_64 VM (slow, emulated)
./scripts/launch_windows_x86_64_macos.sh

# In another terminal, launch Haywire
./build/haywire
```

### Linux Host
```bash
# Launch Ubuntu x86_64 VM
./scripts/launch_linux_x86_64_linux.sh

# Or launch Windows 11 VM
./scripts/launch_windows_x86_64_linux.sh

# In another terminal, launch Haywire
./build/haywire
```

## Archive

Old/obsolete scripts have been moved to `archive/` for historical reference:
- Various Alpine Linux variants
- VLC-specific test configurations
- Old QEMU configurations (clean, overlay, TCG, etc.)
- GDB debugging variants
- Deployment scripts

These are kept for reference but are not maintained.

## Notes

- All scripts use memory-backend-file for zero-copy memory access
- QMP and QEMU Monitor ports are consistent (4445/4444)
- SSH is always forwarded to localhost:2222
- Custom QEMU builds are used when available for enhanced features
