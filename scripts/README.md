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

**`launch_windows_x86_64_macos.sh`** - Windows 11 on macOS (Apple Silicon) ✅ **WORKING**
- **Guest OS**: Windows 11 (x86_64)
- **Host Platform**: macOS (Apple Silicon M1/M2/M3)
- **Acceleration**: TCG (software emulation)
- **Use Case**: Testing Windows kernel introspection on macOS
- **Requirements**:
  - QEMU (`brew install qemu`)
  - swtpm (`brew install swtpm`) - for TPM 2.0 emulation
  - Existing Windows 11 disk image (x86_64)
  - Linux OVMF firmware files in `firmware/` directory
  - Matching VARS file from original Windows setup
  - 8GB RAM minimum
- **Features**:
  - TPM 2.0 emulation (required for Windows 11)
  - memory-backend-file at `/tmp/haywire-vm-mem`
  - QMP on port 4445
  - QEMU Monitor on port 4444
  - Native QEMU window (no VNC - better responsiveness)
  - 129 processes discovered via Windows EPROCESS scanning
- **Performance**:
  - CPU operations slower (TCG emulation)
  - Display/interaction fast (native window, no VNC)
  - Very usable for testing and development
  - For heavy workloads, use Linux host with KVM instead

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

| Guest OS | Host Platform | Script | Acceleration | Status |
|----------|---------------|--------|--------------|--------|
| Linux ARM64 | macOS (Apple Silicon) | `launch_linux_arm64_macos.sh` | HVF | ✅ Fast |
| Linux x86_64 | Linux (Intel/AMD) | `launch_linux_x86_64_linux.sh` | KVM | ✅ Fast |
| Windows 11 x86_64 | Linux (Intel/AMD) | `launch_windows_x86_64_linux.sh` | KVM | ✅ Fast |
| Windows 11 x86_64 | macOS (Apple Silicon) | `launch_windows_x86_64_macos.sh` | TCG | ✅ Usable |

## Quick Start

### macOS (Apple Silicon)
```bash
# Launch Ubuntu ARM64 VM (fast, native)
./scripts/launch_linux_arm64_macos.sh

# Or launch Windows 11 x86_64 VM (usable, emulated)
./scripts/launch_windows_x86_64_macos.sh

# In another terminal, launch Haywire
# For Linux guest:
./build/haywire

# For Windows guest (IMPORTANT: specify OS):
./build/haywire --guest-os windows
```

### Linux Host
```bash
# Launch Ubuntu x86_64 VM
./scripts/launch_linux_x86_64_linux.sh

# Or launch Windows 11 VM
./scripts/launch_windows_x86_64_linux.sh

# In another terminal, launch Haywire
# For Linux guest:
./build/haywire

# For Windows guest (IMPORTANT: specify OS):
./build/haywire --guest-os windows
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

### Windows 11 on macOS Setup Notes

Getting Windows 11 x86_64 to boot on Apple Silicon requires several components:

1. **TPM 2.0 Emulation** - Critical requirement:
   ```bash
   brew install swtpm
   ```
   Windows 11 requires TPM 2.0. Without it, Windows enters automatic repair loop.

2. **Linux OVMF Firmware** - Must use firmware from Linux setup:
   - Place `OVMF_CODE.fd` (3.5MB) in `firmware/` directory
   - Place `OVMF_VARS.fd` template in `firmware/` directory
   - Combined size ~4MB (under 8MB QEMU limit)
   - macOS Homebrew firmware files are too large (>8MB combined)

3. **VARS File** - Boot configuration from original Windows setup:
   - Copy `windows11_VARS.fd` from working Windows host
   - Place in `vms/` directory
   - Contains UEFI boot entries and settings

4. **QEMU Configuration** - Matches Linux script:
   - `-cpu max` for full CPU feature support in TCG
   - `-vga std` for standard VGA (more compatible than virtio)
   - `-usb` for simple USB controller
   - AHCI/SATA disk controller (UEFI can detect without drivers)

5. **Guest OS Hint** - Must specify when running Haywire:
   ```bash
   ./build/haywire --guest-os windows
   ```
   Without this, Haywire defaults to Linux and tries to find task_structs instead of EPROCESS.

**Performance:** TCG emulation is slower for CPU operations but display/interaction is very responsive due to native QEMU window (no VNC overhead). Suitable for testing and development work.
