#!/bin/bash

# Windows 11 Installation Script
# Run this for fresh install, then use launch_windows11.sh for Haywire

VM_DIR="$HOME/haywire-vms"
ISO_DIR="$HOME/haywire-isos"
FW_DIR="$HOME/haywire-firmware"

DISK_IMAGE="$VM_DIR/windows11.qcow2"
WIN11_ISO="$ISO_DIR/Win11_25H2_English_x64.iso"
OVMF_CODE="$FW_DIR/OVMF_CODE.fd"
OVMF_VARS="$VM_DIR/windows11_VARS.fd"
SWTPM_DIR="$VM_DIR/swtpm_win11"

# Create directories
mkdir -p "$VM_DIR" "$ISO_DIR" "$FW_DIR" "$SWTPM_DIR"

# Check for ISO
if [ ! -f "$WIN11_ISO" ]; then
    echo "ERROR: Windows 11 ISO not found at: $WIN11_ISO"
    echo ""
    echo "Please download from: https://www.microsoft.com/software-download/windows11"
    echo "Then copy to: $WIN11_ISO"
    exit 1
fi

# Setup OVMF firmware with Secure Boot support (required for Windows 11)
if [ ! -f "$OVMF_CODE" ]; then
    echo "Copying OVMF Secure Boot firmware..."
    cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd "$OVMF_CODE"
fi
if [ ! -f "$OVMF_VARS" ]; then
    # Use ms (Microsoft) vars which have Secure Boot keys pre-enrolled
    cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd "$OVMF_VARS"
fi

# Create disk if needed
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating 60GB disk image..."
    qemu-img create -f qcow2 "$DISK_IMAGE" 60G
fi

# Start TPM emulator
echo "Starting TPM 2.0 emulator..."
swtpm socket --tpmstate dir="$SWTPM_DIR" \
    --ctrl type=unixio,path="$SWTPM_DIR/swtpm-sock" \
    --tpm2 \
    --log level=0 &
SWTPM_PID=$!
sleep 1

echo ""
echo "=== Starting Windows 11 Installation ==="
echo "Disk: $DISK_IMAGE"
echo "ISO: $WIN11_ISO"
echo ""
echo "INSTALLATION TIPS:"
echo "- To bypass Microsoft account: Shift+F10 -> OOBE\\BYPASSNRO"
echo "- Select 'I don't have internet' after reboot"
echo ""

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
echo "Installation session ended."
