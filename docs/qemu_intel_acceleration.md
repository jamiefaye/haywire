# QEMU Works Great on Intel - Just Use the Right Flags!

## The Problem You Encountered

> "QEMU does a bad job of running on Intel stuff. It insists on emulating an Intel CPU on an Intel CPU. I have not been able to install the Intel Ubuntu on QEMU yet - it just runs real slow and seems to hang after awhile."

**This is a common mistake!** QEMU defaults to TCG (Tiny Code Generator) which is pure software emulation. But QEMU has excellent hardware acceleration support on Intel - you just need to enable it.

## The Solution

Use the `-accel` flag with the right accelerator for your platform:

| Platform | Command | Accelerator Used |
|----------|---------|------------------|
| **Intel macOS** | `qemu-system-x86_64 -accel hvf` | Hypervisor.framework |
| **Intel Linux** | `qemu-system-x86_64 -accel kvm` | Kernel Virtual Machine |
| **Intel Windows** | `qemu-system-x86_64 -accel whpx` | Windows Hypervisor Platform |

## Performance Comparison

| Mode | Speed | When It Happens |
|------|-------|-----------------|
| **TCG (emulation)** | 10-100x slower | When you forget `-accel` |
| **HVF/KVM/WHPX** | Native speed | When you use `-accel` correctly |

## Example: Before and After

### Before (SLOW - emulation):
```bash
qemu-system-x86_64 \
    -M q35 \
    -cpu qemu64 \
    -m 4G \
    -drive if=virtio,format=qcow2,file=ubuntu.qcow2
```
**Result:** Takes 30+ minutes to boot, hangs frequently, unusable

### After (FAST - native):
```bash
qemu-system-x86_64 \
    -M q35 \
    -accel hvf \        # <-- THE MAGIC FLAG
    -cpu host \         # <-- Use all host CPU features
    -m 4G \
    -drive if=virtio,format=qcow2,file=ubuntu.qcow2
```
**Result:** Boots in 30 seconds, runs at native speed, perfectly usable

## Why VMware Doesn't Help

VMware Fusion/Workstation don't support live memory introspection:
- No equivalent to QEMU's `memory-backend-file`
- Only snapshot-based memory dumps (not live)
- Would need to patch VMware (closed source)

QEMU is actually **better** for Haywire because:
- Live memory access via memory-backend-file
- QMP interface for kernel introspection
- Open source (we control it!)
- Works identically on macOS/Linux/Windows

## Platform-Specific Setup

### Intel macOS

**Install QEMU:**
```bash
brew install qemu
```

**Launch script:**
```bash
scripts/launch_ubuntu_x86_64_macos.sh
```

**Key flags:**
- `-accel hvf` - Uses Apple's Hypervisor.framework
- `-cpu host` - Exposes all CPU features to guest

### Intel Linux

**Install QEMU with KVM:**
```bash
sudo apt-get install qemu-kvm
sudo modprobe kvm kvm_intel  # or kvm_amd for AMD
sudo usermod -aG kvm $USER
```

**Launch script:**
```bash
scripts/launch_ubuntu_x86_64_linux.sh
```

**Key flags:**
- `-accel kvm` - Uses Linux KVM
- `-cpu host` - Exposes all CPU features to guest

**Verify KVM works:**
```bash
ls -l /dev/kvm  # Should exist
kvm-ok          # Should say "KVM acceleration can be used"
```

### Intel Windows

**⚠️ WHPX Has Known Issues with Linux Guests**

WHPX (Windows Hypervisor Platform) has persistent bugs when running Linux guests:
- MSI injection failures (error messages spam)
- MMIO emulation errors
- Requires workaround: `-accel whpx,kernel-irqchip=off`
- Still unstable even with workarounds (as of 2024)

**❌ NOT RECOMMENDED for production use**

**✅ RECOMMENDED: WSL2 + KVM (Windows 11)**

Much better performance and stability using KVM in WSL2:

```powershell
# PowerShell as admin:
wsl --install -d Ubuntu-24.04

# Enable nested virtualization for WSL2:
Set-VMProcessor -VMName WSL -ExposeVirtualizationExtensions $true
```

**Inside WSL2:**
```bash
# Install KVM
sudo apt-get update
sudo apt-get install -y qemu-kvm

# Fix /dev/kvm permissions (persist across reboots)
sudo tee /etc/wsl.conf > /dev/null << 'EOF'
[boot]
command = "chmod 666 /dev/kvm"
EOF

# Restart WSL2 to apply:
# (In PowerShell): wsl --shutdown

# Verify KVM works:
ls -l /dev/kvm  # Should show rw-rw-rw-

# Use Linux launch script:
cd /mnt/c/Users/YourName/haywire
./scripts/launch_ubuntu_x86_64_linux.sh
```

**Why WSL2+KVM is better:**
- Native KVM performance (same as Linux)
- No WHPX bugs
- Full Linux environment for development
- Access Windows files via `/mnt/c/`

## Verification

After launching, check QEMU output for acceleration:

**Good (accelerated):**
```
accel: hvf
```

**Bad (emulation):**
```
accel: TCG
```

If you see TCG, acceleration failed. Check:
- Virtualization enabled in BIOS (VT-x/AMD-V)
- Hypervisor platform enabled (Windows)
- User in kvm group (Linux)
- `/dev/kvm` exists (Linux)

## Memory Backend File

All three scripts include:
```bash
-object memory-backend-file,id=mem,size=4G,mem-path=/tmp/haywire-vm-mem,share=on \
-numa node,memdev=mem
```

This creates the live memory file that Haywire reads from. Works identically on all platforms!

## Why This Wasn't Obvious

QEMU's default behavior is confusing:
1. Without `-accel`, it falls back to TCG (silent failure)
2. No warning that you're emulating instead of accelerating
3. Just becomes extremely slow

The QEMU docs assume you know to use acceleration, so they don't emphasize it enough.

## Summary

✅ **QEMU works great on Intel with proper acceleration**
✅ **Same memory-backend-file feature on all platforms**
✅ **Same QMP interface on all platforms**
✅ **Native performance when configured correctly**

❌ **VMware doesn't support live memory introspection**
❌ **VirtualBox only supports snapshots**

**Recommendation:** Use QEMU with HVF/KVM/WHPX on Intel hardware. It's the best solution for Haywire.
