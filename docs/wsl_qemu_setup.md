# WSL and QEMU Setup Guide for Haywire

This guide covers setting up Windows Subsystem for Linux (WSL) with QEMU for running Haywire on Windows.

## Prerequisites

- Windows 10 (version 2004+) or Windows 11
- Administrator access
- At least 16GB RAM recommended (8GB minimum)
- 100GB+ free disk space

## Step 1: Install WSL2

Open PowerShell as Administrator and run:

```powershell
wsl --install
```

This installs WSL2 with Ubuntu by default. Restart your computer when prompted.

After restart, Ubuntu will launch automatically to complete setup. Create a username and password when prompted.

### Verify WSL2 is Working

```powershell
wsl --version
```

You should see WSL version 2.x.

## Step 2: Update Ubuntu

Open WSL (type `wsl` in PowerShell or search for "Ubuntu" in Start menu):

```bash
sudo apt update && sudo apt upgrade -y
```

## Step 3: Install QEMU and Dependencies

```bash
sudo apt install -y \
    qemu-system-x86 \
    qemu-utils \
    ovmf \
    swtpm \
    swtpm-tools
```

This installs:
- **qemu-system-x86**: x86/x64 emulator with KVM support
- **qemu-utils**: Tools like qemu-img for disk management
- **ovmf**: UEFI firmware (required for Windows 11)
- **swtpm**: Software TPM 2.0 emulator (required for Windows 11)

### Verify QEMU Installation

```bash
qemu-system-x86_64 --version
qemu-system-x86_64 -display help
```

The display help should show `gtk` and/or `sdl` options for native window support via WSLg.

## Step 4: Enable KVM Acceleration

WSL2 supports KVM for near-native VM performance. Check if it's available:

```bash
ls -la /dev/kvm
```

If you see the device, add your user to the kvm group:

```bash
sudo usermod -aG kvm $USER
```

Log out and back into WSL for the group change to take effect:

```bash
exit
```

Then reopen WSL and verify:

```bash
groups | grep kvm
```

### Troubleshooting KVM

If `/dev/kvm` doesn't exist:

1. Ensure virtualization is enabled in your BIOS/UEFI (Intel VT-x or AMD-V)
2. Check that Hyper-V is enabled in Windows (WSL2 uses it):
   ```powershell
   # Run in PowerShell as Admin
   dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
   ```
3. Restart Windows

## Step 5: Set Up Directory Structure

Create directories for VM files on native WSL storage (much faster than /mnt/c/):

```bash
mkdir -p ~/haywire-vms
mkdir -p ~/haywire-isos
mkdir -p ~/haywire-firmware
```

Copy OVMF firmware to your firmware directory:

```bash
cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/haywire-firmware/OVMF_CODE.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/haywire-firmware/OVMF_VARS.fd
```

## Step 6: Build Haywire for WSL

Clone or access the Haywire repository. If it's on your Windows drive:

```bash
cd /mnt/c/Users/YOUR_USERNAME/haywire
```

Install build dependencies:

```bash
sudo apt install -y \
    build-essential \
    cmake \
    libgl1-mesa-dev \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libcapstone-dev
```

Build Haywire:

```bash
mkdir -p build-wsl
cd build-wsl
cmake ..
make -j$(nproc)
```

## Step 7: Test the Setup

Verify everything works:

```bash
# Check QEMU with KVM
qemu-system-x86_64 -enable-kvm -cpu host -m 512M -display gtk &
# A blank QEMU window should appear. Close it.

# Check Haywire builds
./build-wsl/haywire --help
```

## Performance Tips

### Use Native WSL Storage

Always store VM disk images on native WSL storage (`~/`), not on `/mnt/c/`. The 9P filesystem used for Windows drive access is very slow for disk I/O.

```bash
# Good - native WSL storage
~/haywire-vms/windows11.qcow2

# Bad - Windows filesystem via 9P (very slow)
/mnt/c/Users/jamie/vms/windows11.qcow2
```

### Memory Allocation

WSL2 by default limits memory to 50% of system RAM. For better VM performance, create or edit `%USERPROFILE%\.wslconfig`:

```ini
[wsl2]
memory=12GB
processors=4
```

Then restart WSL:
```powershell
wsl --shutdown
```

## Next Steps

- [Windows 11 VM Setup Guide](windows11_vm_setup.md) - Create a Windows 11 VM for Haywire
- [Ubuntu ARM64 VM Setup Guide](vm_setup_guide.md) - Create a Linux VM for Haywire

## Troubleshooting

### "Display not available" or No Window Appears

WSLg may not be working. Check:
```bash
echo $DISPLAY
```

Should show something like `:0`. If empty, try:
```bash
export DISPLAY=:0
```

Or fall back to VNC in your launch script:
```bash
-vnc :1
```
Then connect with a VNC client to `localhost:5901`.

### QEMU Crashes or Freezes

- Ensure KVM is working: `ls /dev/kvm`
- Check available memory: `free -h`
- Try reducing VM memory allocation

### Slow Performance

- Move disk images to native WSL storage
- Ensure KVM is enabled (`-enable-kvm` in QEMU command)
- Use `-cpu host` for best CPU performance
- Increase WSL memory limit in `.wslconfig`
