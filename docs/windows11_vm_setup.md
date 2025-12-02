# Windows 11 VM Setup Guide for Haywire

This guide covers creating a Windows 11 x86_64 virtual machine from scratch for use with Haywire memory introspection.

## Prerequisites

- WSL2 with QEMU installed (see [WSL and QEMU Setup Guide](wsl_qemu_setup.md))
- Windows 11 ISO image (approximately 7GB for 25H2)
- At least 60GB free disk space
- 8GB+ RAM available for the VM

## Step 1: Download Windows 11 ISO

1. Go to [Microsoft's Windows 11 Download Page](https://www.microsoft.com/software-download/windows11)
2. Under "Download Windows 11 Disk Image (ISO)", select "Windows 11 (multi-edition ISO)"
3. Choose your language and click Download
4. Save the ISO (approximately 7GB for 25H2)

Move the ISO to your WSL storage:

```bash
mkdir -p ~/haywire-isos
cp /mnt/c/Users/YOUR_USERNAME/Downloads/Win11_*.iso ~/haywire-isos/
```

## Step 2: Set Up OVMF Firmware

Windows 11 requires UEFI boot with Secure Boot. Use the Microsoft-signed OVMF files:

```bash
mkdir -p ~/haywire-firmware ~/haywire-vms

# Copy Secure Boot enabled firmware
cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd ~/haywire-firmware/OVMF_CODE.fd

# Copy VARS with Microsoft keys pre-enrolled (required for Secure Boot)
cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd ~/haywire-vms/windows11_VARS.fd
```

**Important**: Use `OVMF_VARS_4M.ms.fd` (not plain `OVMF_VARS_4M.fd`) - it has Microsoft's Secure Boot keys pre-enrolled which Windows 11 requires.

## Step 3: Create Virtual Disk

Create a 60GB qcow2 disk image:

```bash
qemu-img create -f qcow2 ~/haywire-vms/windows11.qcow2 60G
```

## Step 4: Prepare TPM 2.0 Emulation

Windows 11 requires TPM 2.0. Install swtpm and create a directory for TPM state:

```bash
sudo apt-get install swtpm swtpm-tools
mkdir -p ~/haywire-vms/swtpm_win11
```

## Step 5: Run Installation

Use the installation script from the repository:

```bash
wsl bash /mnt/c/Users/YOUR_USERNAME/haywire/scripts/install_windows11.sh
```

Or create your own `~/install_windows11.sh`:

```bash
#!/bin/bash

# Paths
VM_DIR="$HOME/haywire-vms"
ISO_DIR="$HOME/haywire-isos"
FW_DIR="$HOME/haywire-firmware"

DISK_IMAGE="$VM_DIR/windows11.qcow2"
WIN11_ISO="$ISO_DIR/Win11_25H2_English_x64.iso"  # Adjust filename as needed
OVMF_CODE="$FW_DIR/OVMF_CODE.fd"
OVMF_VARS="$VM_DIR/windows11_VARS.fd"
SWTPM_DIR="$VM_DIR/swtpm_win11"

# Start TPM emulator
swtpm socket --tpmstate dir="$SWTPM_DIR" \
    --ctrl type=unixio,path="$SWTPM_DIR/swtpm-sock" \
    --tpm2 \
    --log level=0 &
SWTPM_PID=$!
sleep 1

# Launch QEMU for installation
# NOTE: -cpu host,-vmx,-hypervisor is critical for nested virtualization (WSL2)
# NOTE: -nic none disables networking to allow local account creation
qemu-system-x86_64 \
    -enable-kvm \
    -machine q35 \
    -cpu host,-vmx,-hypervisor,+invtsc \
    -smp 4 \
    -m 8G \
    \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$OVMF_VARS" \
    \
    -device ahci,id=ahci \
    -drive "file=$DISK_IMAGE,if=none,id=disk,format=qcow2" \
    -device ide-hd,drive=disk,bus=ahci.0 \
    \
    -drive "file=$WIN11_ISO,if=none,id=cdrom,media=cdrom,readonly=on" \
    -device ide-cd,drive=cdrom,bus=ahci.1 \
    \
    -boot order=d,menu=on \
    \
    -chardev "socket,id=chrtpm,path=$SWTPM_DIR/swtpm-sock" \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    \
    -vga std \
    -display gtk,grab-on-hover=on \
    \
    -usb \
    -device usb-kbd \
    -device usb-tablet \
    \
    -nic none \
    \
    -name "Windows-11-Install"

# Cleanup
kill $SWTPM_PID 2>/dev/null
```

### Critical QEMU Options Explained

| Option | Purpose |
|--------|---------|
| `-cpu host,-vmx,-hypervisor,+invtsc` | Use host CPU but disable VT-x passthrough (prevents triple fault in nested virtualization) |
| `-nic none` | Disable all networking (forces "I don't have internet" option in OOBE) |
| `OVMF_VARS_4M.ms.fd` | UEFI variables with Microsoft Secure Boot keys pre-enrolled |
| `-device tpm-tis` | TPM 2.0 device (Windows 11 requirement) |

## Step 6: Windows 11 Installation Process

1. The VM will boot from the ISO. Press any key when prompted to boot from CD.

2. Select language, time, and keyboard settings. Click "Next".

3. Click "Install now".

4. Click "I don't have a product key" (you can activate later).

5. Select "Windows 11 Pro" (or your preferred edition). Click "Next".

6. Accept the license terms. Click "Next".

7. Select "Custom: Install Windows only (advanced)".

8. Select the unallocated drive space and click "Next".

9. Wait for installation to complete (15-30 minutes). The VM will reboot several times.

### Creating a Local Account (No Microsoft Account)

Because `-nic none` disables networking, Windows OOBE will show:

1. "Let's connect you to a network" screen
2. Click **"I don't have internet"**
3. Click **"Continue with limited setup"**
4. Enter your username and password
5. Answer 3 security questions
6. Configure privacy settings (disable all for a test VM)

**If you still see "Set up for work or school"**: The VM somehow has network access. Close QEMU and verify `-nic none` is in your script.

### Complete Initial Setup

- Choose your privacy settings (disable all for a test VM)
- Wait for Windows to finalize setup (may take several minutes at "Hi" screen)

## Step 7: Launch with Haywire Support

After installation is complete, use the Haywire-enabled launch script:

```bash
wsl bash /mnt/c/Users/YOUR_USERNAME/haywire/scripts/launch_windows11.sh
```

Key differences from installation script:
- Adds `memory-backend-file` for Haywire introspection
- Adds QMP port (4445) and monitor port (4444)
- Removes CD-ROM boot priority
- Still uses `-nic none` (no network)

## Step 8: Running with Haywire

1. Start the VM:
   ```bash
   wsl bash /mnt/c/Users/YOUR_USERNAME/haywire/scripts/launch_windows11.sh
   ```

2. Wait for Windows to boot to the desktop.

3. In another terminal, start Haywire:
   ```bash
   wsl bash -c "cd /mnt/c/Users/YOUR_USERNAME/haywire && ./build-wsl/haywire --guest-os windows"
   ```

4. In Haywire, click "Connect" to connect to QEMU via QMP.

5. Run kernel discovery to find Windows processes.

## Optional: Install VirtIO Drivers

For better disk and network performance, you can install VirtIO drivers:

1. Download the VirtIO ISO from [Fedora](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso)

2. After Windows is installed, attach the VirtIO ISO and install drivers from Device Manager.

3. Update your launch script to use VirtIO disk instead of AHCI:
   ```bash
   -drive "file=$DISK_IMAGE,if=none,id=disk,format=qcow2" \
   -device virtio-blk-pci,drive=disk \
   ```

## Troubleshooting

### Triple Fault / Register Dump with Error 0x0

This happens when running QEMU inside WSL2 (nested virtualization). The fix:

```bash
-cpu host,-vmx,-hypervisor,+invtsc
```

The `-vmx` disables VT-x passthrough and `-hypervisor` hides the hypervisor bit from the guest.

### "This PC can't run Windows 11" Error

TPM is not detected. Verify swtpm is running:
```bash
ps aux | grep swtpm
```

Also ensure you're using Secure Boot firmware (`OVMF_CODE_4M.secboot.fd` and `OVMF_VARS_4M.ms.fd`).

### Stuck at "Set up for work or school" / Microsoft Account Required

Network is still active. Ensure `-nic none` is in your QEMU command line. If using the repository scripts, update to the latest version.

### Keyboard Stops Working During OOBE

USB devices can glitch in QEMU. Try:
1. Click inside the QEMU window
2. Press Ctrl+Alt+G to toggle grab
3. If still stuck, close and restart QEMU (progress is saved in qcow2)

### VM is Very Slow

- Ensure KVM is enabled: check for `-enable-kvm` in ps output
- Move disk image to native WSL storage (not /mnt/c/)
- Use `-cpu host,...` not `-cpu qemu64` or similar

### Mouse Not Tracking Properly

The `-device usb-tablet` provides absolute positioning. If issues persist, try clicking inside the VM window first, or press Ctrl+Alt+G to grab/release mouse.

## Disk Management

### Create a Snapshot (Backup)

```bash
qemu-img snapshot -c "clean_install" ~/haywire-vms/windows11.qcow2
```

### List Snapshots

```bash
qemu-img snapshot -l ~/haywire-vms/windows11.qcow2
```

### Restore Snapshot

```bash
qemu-img snapshot -a "clean_install" ~/haywire-vms/windows11.qcow2
```

### Check Disk Size

```bash
qemu-img info ~/haywire-vms/windows11.qcow2
```

### Reset to Fresh Install

If you need to start over completely:

```bash
rm ~/haywire-vms/windows11.qcow2 ~/haywire-vms/windows11_VARS.fd
rm -rf ~/haywire-vms/swtpm_win11/*
qemu-img create -f qcow2 ~/haywire-vms/windows11.qcow2 60G
cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd ~/haywire-vms/windows11_VARS.fd
```
