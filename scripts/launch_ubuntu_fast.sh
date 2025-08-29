#!/bin/bash

# Optimized Ubuntu VM for better performance

UBUNTU_ISO="/Users/jamie/haywire/ubuntu-24.04.3-desktop-amd64.iso"
DISK_IMAGE="ubuntu_vlc.qcow2"

# Performance optimizations
MEMORY="4G"
CORES="4"  # Increased cores
QMP_PORT=4445
MONITOR_PORT=4444

echo "=== Launching Ubuntu (Optimized) ==="
echo "Performance tweaks enabled"
echo ""

# Launch with performance optimizations
qemu-system-x86_64 \
    -m $MEMORY \
    -smp $CORES \
    -cpu host \
    -drive file="$DISK_IMAGE",format=qcow2,if=virtio,cache=writeback \
    -cdrom "$UBUNTU_ISO" \
    -boot c \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -netdev user,id=net0 \
    -device virtio-net-pci,netdev=net0 \
    -vga std \
    -usb -device usb-tablet \
    -display default,show-cursor=on \
    -machine q35 \
    -device virtio-balloon \
    -name "Ubuntu-Fast"

echo "VM shut down."