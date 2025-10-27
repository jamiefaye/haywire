# Running Haywire on Intel x86_64 Systems

## The Problem

When running QEMU on Intel hardware, you MUST use hardware acceleration or it will be extremely slow (TCG emulation). The key is using the right `-accel` option for your platform.

## Solutions by Platform

### Intel macOS
```bash
qemu-system-x86_64 -accel hvf -cpu host ...
```
- Uses **Hypervisor.framework** (Apple's native hypervisor)
- NATIVE performance, no emulation
- Script: `scripts/launch_ubuntu_x86_64_macos.sh`

### Intel Linux
```bash
qemu-system-x86_64 -accel kvm -cpu host ...
```
- Uses **KVM** (Kernel Virtual Machine)
- NATIVE performance, no emulation
- Requires: `/dev/kvm` exists, user in `kvm` group
- Script: `scripts/launch_ubuntu_x86_64_linux.sh`

### Intel Windows (WSL2 + KVM - RECOMMENDED)
```bash
qemu-system-x86_64 -accel kvm -cpu host ...
```
- Uses **KVM** inside WSL2 (Windows Subsystem for Linux)
- NATIVE performance, no emulation
- Requires: Windows 11, WSL2 with nested virtualization enabled
- Script: `scripts/launch_ubuntu_x86_64_linux.sh` (inside WSL2)

### Intel Windows (WHPX - NOT RECOMMENDED)
```bash
qemu-system-x86_64 -accel whpx -cpu host ...
```
- Uses **WHPX** (Windows Hypervisor Platform)
- ⚠️ **Has known bugs with Linux guests** (MSI injection failures, MMIO errors)
- Requires workaround: `-accel whpx,kernel-irqchip=off`
- Still unstable even with workarounds
- Only use if you can't use WSL2+KVM
- Script: `scripts/launch_ubuntu_x86_64_windows.bat`

## Memory Backend File Paths

### macOS/Linux
```
/tmp/haywire-vm-mem
```

### Windows
```
\\.\pipe\haywire-vm-mem
```
Windows uses named pipes instead of regular files for shared memory.

## Key Differences from ARM64

| Feature | ARM64 (current) | x86_64 (Intel) |
|---------|----------------|----------------|
| QEMU binary | `qemu-system-aarch64` | `qemu-system-x86_64` |
| Machine type | `-M virt` | `-M q35` |
| Accelerator (Mac) | `-accel hvf` | `-accel hvf` |
| Accelerator (Linux) | N/A (emulated) | `-accel kvm` |
| Accelerator (Windows) | N/A | `-accel whpx` |
| CPU | `-cpu cortex-a72` | `-cpu host` |
| UEFI firmware | `edk2-aarch64-code.fd` | `edk2-x86_64-code.fd` |
| Ubuntu ISO | `ubuntu-*-arm64.iso` | `ubuntu-*-amd64.iso` |

## Performance Expectations

With proper acceleration:
- **Excellent**: KVM on Linux (best performance)
- **Excellent**: WSL2+KVM on Windows 11 (same as native Linux)
- **Excellent**: HVF on macOS (nearly as good as KVM)
- **Poor/Buggy**: WHPX on Windows (avoid for Linux guests)

Without acceleration (TCG emulation):
- **Unusable**: 10-100x slower, will hang/timeout

## Kernel Profile Considerations

x86_64 and ARM64 have **different kernel structure offsets**! You'll need to create a new kernel profile for x86_64:

1. Boot x86_64 Ubuntu VM
2. Install pahole: `sudo apt-get install dwarves`
3. Extract offsets:
   ```bash
   pahole -C task_struct /sys/kernel/btf/vmlinux > /tmp/p.txt
   pahole -C mm_struct /sys/kernel/btf/vmlinux >> /tmp/p.txt
   pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /tmp/p.txt
   ```
4. Generate profile:
   ```bash
   python3 scripts/create_kernel_profile.py < /tmp/p.txt > profiles/ubuntu-x86_64.json
   ```
5. Load profile in Haywire:
   ```bash
   ./haywire --kernel-profile profiles/ubuntu-x86_64.json
   ```

## Testing Checklist

- [ ] Download x86_64 Ubuntu ISO
- [ ] Enable virtualization in BIOS (VT-x for Intel)
- [ ] Enable hypervisor platform (WHPX on Windows, KVM on Linux)
- [ ] Launch VM with correct accelerator
- [ ] Verify fast performance (should boot in seconds, not minutes)
- [ ] Create x86_64 kernel profile
- [ ] Test Haywire with x86_64 VM

## Troubleshooting

### "accel: unknown accelerator: hvf/kvm/whpx"
- macOS: QEMU may be old, try: `brew upgrade qemu`
- Linux: Install kvm-enabled QEMU: `sudo apt-get install qemu-kvm`
- Windows: Use WSL2+KVM instead of WHPX (see setup above)

### Still slow even with acceleration
- Check QEMU output for "accel: TCG" (means acceleration failed)
- Verify `/dev/kvm` exists (Linux)
- Check BIOS for VT-x/AMD-V enabled
- Try `-cpu host` instead of specific CPU model

### "Could not access KVM kernel module"
```bash
# Linux only
sudo modprobe kvm kvm_intel  # or kvm_amd for AMD CPUs
sudo usermod -aG kvm $USER
# Log out and back in
```

### Windows: "WHPX: Failed to setup partition" or MSI injection errors
- WHPX has known bugs with Linux guests - **use WSL2+KVM instead**
- If you must use WHPX, try: `-accel whpx,kernel-irqchip=off`
- Still expect instability and error messages

### Windows: Setting up WSL2 + KVM (RECOMMENDED)

Windows 11 supports KVM in WSL2 with excellent performance:

1. **Install WSL2:**
   ```powershell
   # PowerShell as admin:
   wsl --install -d Ubuntu-24.04
   ```

2. **Enable nested virtualization:**
   ```powershell
   # PowerShell as admin:
   Set-VMProcessor -VMName WSL -ExposeVirtualizationExtensions $true
   ```

3. **Inside WSL2, install KVM:**
   ```bash
   sudo apt-get update
   sudo apt-get install -y qemu-kvm

   # Fix /dev/kvm permissions:
   sudo tee /etc/wsl.conf > /dev/null << 'EOF'
   [boot]
   command = "chmod 666 /dev/kvm"
   EOF

   # Restart WSL2 (in PowerShell):
   # wsl --shutdown
   ```

4. **Use Linux launch script:**
   ```bash
   cd /mnt/c/Users/YourName/haywire
   ./scripts/launch_ubuntu_x86_64_linux.sh
   ```

This gives you native KVM performance on Windows!

## Summary

**Bottom line**: QEMU works great on Intel with proper acceleration. The slowness you experienced was likely from missing `-accel` flag or disabled virtualization. Use the provided scripts and you'll get native x86_64 performance with full Haywire support.
