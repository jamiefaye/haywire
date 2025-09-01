#!/bin/bash

# Launch ARM64 Ubuntu with GDB server enabled for faster memory access

UBUNTU_ISO="/Users/jamie/haywire/ubuntu-25.04-desktop-arm64.iso"
DISK_IMAGE="../vms/ubuntu_arm64.qcow2"

# QEMU configuration
MEMORY="4G"
CORES="4"
QMP_PORT=4445
MONITOR_PORT=4444
GDB_PORT=1234

echo "=== ARM64 Ubuntu with GDB Server ==="
echo "GDB server enabled on port $GDB_PORT for fast memory access"
echo ""

# Create disk if needed
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE (20G)"
    qemu-img create -f qcow2 "$DISK_IMAGE" 20G
fi

# Check if ISO exists
if [ ! -f "$UBUNTU_ISO" ]; then
    echo "ARM64 Ubuntu ISO not found!"
    echo ""
    echo "Download from one of these:"
    echo "Desktop: https://cdimage.ubuntu.com/jammy/daily-live/current/jammy-desktop-arm64.iso"
    echo "Server: https://cdimage.ubuntu.com/releases/22.04/release/ubuntu-22.04.3-live-server-arm64.iso"
    echo ""
    echo "Save as: $UBUNTU_ISO"
    exit 1
fi

echo "Starting QEMU with HVF acceleration and GDB server..."
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT, GDB=$GDB_PORT"

# Find UEFI firmware
UEFI_CODE="/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
if [ ! -f "$UEFI_CODE" ]; then
    UEFI_CODE="/usr/local/share/qemu/edk2-aarch64-code.fd"
fi

if [ ! -f "$UEFI_CODE" ]; then
    echo "ERROR: UEFI firmware not found!"
    echo "Try: brew reinstall qemu"
    exit 1
fi

echo "Using UEFI firmware: $UEFI_CODE"

# Launch with HVF acceleration and GDB server
qemu-system-aarch64 \
    -M virt,highmem=on \
    -accel hvf \
    -cpu host \
    -m $MEMORY \
    -smp $CORES \
    -bios "$UEFI_CODE" \
    -device virtio-gpu-pci \
    -display default,show-cursor=on \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -device intel-hda \
    -device hda-duplex \
    -drive if=virtio,format=qcow2,file="$DISK_IMAGE" \
    -cdrom "$UBUNTU_ISO" \
    -boot d \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -gdb tcp::$GDB_PORT \
    -chardev socket,path=/tmp/qga.sock,server,nowait,id=qga0 \
    -device virtio-serial \
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -serial stdio \
    -name "Ubuntu-ARM64-GDB"

echo "VM shut down."