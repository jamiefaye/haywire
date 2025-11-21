#!/bin/bash

# Launch x86_64 Ubuntu on Intel Mac with HVF acceleration
# This uses Hypervisor.framework for NATIVE performance (not emulation!)

UBUNTU_ISO="/Users/jamie/haywire/ubuntu-24.04-desktop-amd64.iso"
DISK_IMAGE="../vms/ubuntu_x86_64.qcow2"

# QEMU configuration
MEMORY="4G"
CORES="4"
QMP_PORT=4445
MONITOR_PORT=4444

echo "=== x86_64 Ubuntu with HVF Acceleration ==="
echo "This should be FAST on Intel Mac - using native CPU!"
echo ""

# Create disk if needed
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE (20G)"
    qemu-img create -f qcow2 "$DISK_IMAGE" 20G
fi

# Check if ISO exists
if [ ! -f "$UBUNTU_ISO" ]; then
    echo "x86_64 Ubuntu ISO not found!"
    echo ""
    echo "Download from:"
    echo "Desktop: https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso"
    echo "Server: https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso"
    echo ""
    echo "Save as: $UBUNTU_ISO"
    exit 1
fi

echo "Starting QEMU with HVF acceleration (native Intel)..."
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT"

# Find UEFI firmware for x86_64
UEFI_CODE="/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
if [ ! -f "$UEFI_CODE" ]; then
    UEFI_CODE="/usr/local/share/qemu/edk2-x86_64-code.fd"
fi

if [ ! -f "$UEFI_CODE" ]; then
    echo "WARNING: UEFI firmware not found, using BIOS mode"
    UEFI_CODE=""
fi

# Launch with HVF acceleration (Hypervisor.framework)
# -cpu host = use ALL host CPU features (fastest!)
qemu-system-x86_64 \
    -M q35 \
    -accel hvf \
    -cpu host \
    -m $MEMORY \
    -smp $CORES \
    ${UEFI_CODE:+-bios "$UEFI_CODE"} \
    -device virtio-vga \
    -display default,show-cursor=on \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -device intel-hda \
    -device hda-duplex \
    -drive if=virtio,format=qcow2,file="$DISK_IMAGE" \
    -cdrom "$UBUNTU_ISO" \
    -boot d \
    -object memory-backend-file,id=mem,size=$MEMORY,mem-path=/tmp/haywire-vm-mem,share=on \
    -numa node,memdev=mem \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -chardev socket,path=/tmp/qga.sock,server=on,wait=off,id=qga0 \
    -device virtio-serial \
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -serial stdio \
    -name "Ubuntu-x86_64-HVF"

echo "VM shut down."
