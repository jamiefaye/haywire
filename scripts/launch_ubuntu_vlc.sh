#!/bin/bash

# Launch Ubuntu Desktop with VLC in QEMU for Haywire testing
# This script assumes you have an Ubuntu Desktop ISO or disk image

# Configuration - UPDATE THESE PATHS
UBUNTU_ISO="/Users/jamie/haywire/ubuntu-24.04.3-desktop-amd64.iso"
DISK_IMAGE="ubuntu_vlc.qcow2"

# QEMU configuration
MEMORY="4G"
CORES="2"
QMP_PORT=4445
MONITOR_PORT=4444

# Check for macOS and use appropriate acceleration
if [[ "$OSTYPE" == "darwin"* ]]; then
    ACCEL=""  # No acceleration, will use TCG
    DISPLAY="-display default,show-cursor=on"
    echo "Running on macOS (using software emulation)"
else
    ACCEL="-enable-kvm"
    DISPLAY="-display gtk"
    echo "Running on Linux with KVM acceleration"
fi

echo "=== Launching Ubuntu Desktop for VLC Testing ==="
echo "QMP Port: $QMP_PORT (for Haywire connection)"
echo "Monitor Port: $MONITOR_PORT"
echo ""

# Create disk image if it doesn't exist
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE (20G)"
    qemu-img create -f qcow2 "$DISK_IMAGE" 20G
fi

# Determine boot options
BOOT_OPTS=""
if [ -n "$UBUNTU_ISO" ] && [ -f "$UBUNTU_ISO" ]; then
    echo "Booting from ISO: $UBUNTU_ISO"
    BOOT_OPTS="-cdrom $UBUNTU_ISO -boot c"  # Boot from disk (c) not cdrom (d)
    echo ""
    echo "First boot instructions:"
    echo "1. Install Ubuntu to the virtual disk"
    echo "2. During installation, install third-party software"
    echo "3. After installation, install VLC: sudo apt install vlc"
elif [ -f "$DISK_IMAGE" ]; then
    echo "Booting from disk image: $DISK_IMAGE"
    BOOT_OPTS="-boot c"
else
    echo "ERROR: No Ubuntu ISO configured and no existing disk image"
    echo "Please set UBUNTU_ISO path in this script"
    echo "Download from: https://ubuntu.com/download/desktop"
    exit 1
fi

# Launch QEMU
qemu-system-x86_64 \
    $ACCEL \
    -m $MEMORY \
    -smp $CORES \
    -drive file="$DISK_IMAGE",format=qcow2,if=virtio \
    $BOOT_OPTS \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -vga virtio \
    -usb -device usb-tablet \
    -device AC97 \
    $DISPLAY \
    -name "Ubuntu-VLC-Haywire"

echo ""
echo "VM has been shut down."