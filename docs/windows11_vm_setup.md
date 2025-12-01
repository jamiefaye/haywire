# Windows 11 VM Setup Guide for Haywire

This guide covers creating a Windows 11 x86_64 virtual machine from scratch for use with Haywire memory introspection.

## Prerequisites

- WSL2 with QEMU installed (see [WSL and QEMU Setup Guide](wsl_qemu_setup.md))
- Windows 11 ISO image
- At least 60GB free disk space
- 8GB+ RAM available for the VM

## Step 1: Download Windows 11 ISO

1. Go to [Microsoft's Windows 11 Download Page](https://www.microsoft.com/software-download/windows11)
2. Under "Download Windows 11 Disk Image (ISO)", select "Windows 11 (multi-edition ISO)"
3. Choose your language and click Download
4. Save the ISO (approximately 5-6GB)

Move the ISO to your WSL storage:

```bash
mkdir -p ~/haywire-isos
cp /mnt/c/Users/YOUR_USERNAME/Downloads/Win11_*.iso ~/haywire-isos/
```

## Step 2: Set Up OVMF Firmware

Windows 11 requires UEFI boot. Copy OVMF firmware files:

```bash
mkdir -p ~/haywire-firmware
cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/haywire-firmware/OVMF_CODE.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/haywire-firmware/OVMF_VARS.fd

# Create a per-VM copy of VARS (stores UEFI settings)
cp ~/haywire-firmware/OVMF_VARS.fd ~/haywire-vms/windows11_VARS.fd
```

## Step 3: Create Virtual Disk

Create a 60GB qcow2 disk image:

```bash
qemu-img create -f qcow2 ~/haywire-vms/windows11.qcow2 60G
```

## Step 4: Prepare TPM 2.0 Emulation

Windows 11 requires TPM 2.0. Create a directory for TPM state:

```bash
mkdir -p ~/haywire-vms/swtpm_win11
```

## Step 5: Initial Windows Installation

Create an installation script `~/install_windows11.sh`:

```bash
#!/bin/bash

# Paths
VM_DIR="$HOME/haywire-vms"
ISO_DIR="$HOME/haywire-isos"
FW_DIR="$HOME/haywire-firmware"

DISK_IMAGE="$VM_DIR/windows11.qcow2"
WIN11_ISO="$ISO_DIR/Win11_24H2_English_x64.iso"  # Adjust filename as needed
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
qemu-system-x86_64 \
    -enable-kvm \
    -machine q35 \
    -cpu host \
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
    -display gtk \
    \
    -usb \
    -device usb-kbd \
    -device usb-tablet \
    \
    -name "Windows-11-Install"

# Cleanup
kill $SWTPM_PID 2>/dev/null
```

Make it executable and run:

```bash
chmod +x ~/install_windows11.sh
~/install_windows11.sh
```

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

### Bypassing Microsoft Account Requirement

When you reach the "Let's connect you to a network" screen:

1. Press `Shift + F10` to open Command Prompt
2. Type: `OOBE\BYPASSNRO`
3. Press Enter - the system will reboot
4. After reboot, you'll see "I don't have internet" option - click it
5. Click "Continue with limited setup"
6. Create a local account with username and password

### Complete Initial Setup

- Choose your privacy settings (disable all for a test VM)
- Wait for Windows to finalize setup

## Step 7: Create Haywire Launch Script

After installation is complete, create the Haywire-enabled launch script.

Save this as `/mnt/c/Users/YOUR_USERNAME/haywire/scripts/launch_windows11.sh` or use the existing script in the repository.

Key differences from installation script:
- Adds `memory-backend-file` for Haywire introspection
- Adds QMP and monitor ports
- Removes ISO boot priority

```bash
#!/bin/bash

VM_DIR="$HOME/haywire-vms"
FW_DIR="$HOME/haywire-firmware"
MEMFILE="/tmp/haywire-vm-mem"

DISK_IMAGE="$VM_DIR/windows11.qcow2"
OVMF_CODE="$FW_DIR/OVMF_CODE.fd"
OVMF_VARS="$VM_DIR/windows11_VARS.fd"
SWTPM_DIR="$VM_DIR/swtpm_win11"

# Clean up old memory file
rm -f "$MEMFILE"

# Start TPM emulator
swtpm socket --tpmstate dir="$SWTPM_DIR" \
    --ctrl type=unixio,path="$SWTPM_DIR/swtpm-sock" \
    --tpm2 \
    --log level=0 &
SWTPM_PID=$!
sleep 1

echo "Starting Windows 11 VM with Haywire support..."
echo "Memory file: $MEMFILE"
echo "QMP port: 4445"

qemu-system-x86_64 \
    -enable-kvm \
    -machine q35 \
    -cpu host \
    -smp 4 \
    -m 8G \
    \
    -object "memory-backend-file,id=mem,size=8G,mem-path=$MEMFILE,share=on" \
    -numa "node,memdev=mem" \
    \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$OVMF_VARS" \
    \
    -device ahci,id=ahci \
    -drive "file=$DISK_IMAGE,if=none,id=disk,format=qcow2" \
    -device ide-hd,drive=disk,bus=ahci.0 \
    \
    -chardev "socket,id=chrtpm,path=$SWTPM_DIR/swtpm-sock" \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    \
    -vga std \
    -display gtk \
    \
    -usb \
    -device usb-kbd \
    -device usb-tablet \
    \
    -qmp "tcp:localhost:4445,server,nowait" \
    -monitor "telnet:localhost:4444,server,nowait" \
    \
    -name "Windows-11-Haywire"

# Cleanup
rm -f "$MEMFILE"
kill $SWTPM_PID 2>/dev/null
echo "VM shut down."
```

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

### "This PC can't run Windows 11" Error

This usually means TPM is not detected. Verify swtpm is running:
```bash
ps aux | grep swtpm
```

### VM is Very Slow

- Ensure KVM is enabled: check for `-enable-kvm` in ps output
- Move disk image to native WSL storage (not /mnt/c/)
- Ensure you're using `-cpu host`

### Mouse Not Tracking Properly

The `-device usb-tablet` provides absolute positioning. If issues persist, try clicking inside the VM window first, or press Ctrl+Alt+G to grab/release mouse.

### No Network in VM

Network is intentionally disabled in the Haywire scripts to avoid Windows Update interference. To enable:
```bash
-netdev user,id=net0 \
-device virtio-net-pci,netdev=net0 \
```

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
