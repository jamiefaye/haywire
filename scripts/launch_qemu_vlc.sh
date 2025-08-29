#!/bin/bash

# QEMU launch script for VLC testing with Haywire
# This script launches a QEMU VM with VLC installed for testing memory visualization

# Configuration
QEMU_SYSTEM="qemu-system-x86_64"
MEMORY="2G"
CORES="2"

# Network ports for debugging
QMP_PORT=4445
MONITOR_PORT=4444
VNC_PORT=5901

# ISO/Disk image paths - update these to match your setup
ISO_PATH=""
DISK_IMAGE=""

# Check if running on macOS and use appropriate acceleration
if [[ "$OSTYPE" == "darwin"* ]]; then
    ACCEL="-accel hvf"
else
    ACCEL="-enable-kvm"
fi

echo "Starting QEMU with VLC test environment..."
echo "QMP Port: $QMP_PORT"
echo "Monitor Port: $MONITOR_PORT"
echo "VNC Display: :1 (port $VNC_PORT)"
echo ""

# Launch QEMU
# For now, we'll boot from a Linux ISO with VLC
# You'll need to provide either:
# 1. A Linux ISO with VLC pre-installed, or
# 2. A disk image with an OS and VLC already set up

if [ -n "$DISK_IMAGE" ] && [ -f "$DISK_IMAGE" ]; then
    echo "Using disk image: $DISK_IMAGE"
    
    $QEMU_SYSTEM \
        $ACCEL \
        -m $MEMORY \
        -smp $CORES \
        -drive file="$DISK_IMAGE",format=qcow2 \
        -qmp tcp:localhost:$QMP_PORT,server,nowait \
        -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
        -vnc :1 \
        -device virtio-net-pci,netdev=net0 \
        -netdev user,id=net0,hostfwd=tcp::2222-:22 \
        -display none
        
elif [ -n "$ISO_PATH" ] && [ -f "$ISO_PATH" ]; then
    echo "Booting from ISO: $ISO_PATH"
    echo "Note: You'll need to install VLC after booting"
    
    # Create a temporary disk if needed
    TEMP_DISK="vlc_test.qcow2"
    if [ ! -f "$TEMP_DISK" ]; then
        echo "Creating temporary disk image: $TEMP_DISK"
        qemu-img create -f qcow2 "$TEMP_DISK" 20G
    fi
    
    $QEMU_SYSTEM \
        $ACCEL \
        -m $MEMORY \
        -smp $CORES \
        -cdrom "$ISO_PATH" \
        -drive file="$TEMP_DISK",format=qcow2 \
        -boot d \
        -qmp tcp:localhost:$QMP_PORT,server,nowait \
        -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
        -vnc :1 \
        -device virtio-net-pci,netdev=net0 \
        -netdev user,id=net0,hostfwd=tcp::2222-:22 \
        -display none
        
else
    echo "ERROR: Please configure either ISO_PATH or DISK_IMAGE in this script"
    echo ""
    echo "Option 1: Set ISO_PATH to a Linux distribution ISO"
    echo "  Example: ISO_PATH=\"/path/to/ubuntu-desktop.iso\""
    echo ""
    echo "Option 2: Set DISK_IMAGE to a pre-configured disk image"
    echo "  Example: DISK_IMAGE=\"/path/to/vlc-test.qcow2\""
    echo ""
    echo "Recommended distributions for VLC testing:"
    echo "  - Ubuntu Desktop (includes GUI and easy VLC installation)"
    echo "  - Debian with XFCE"
    echo "  - Fedora Workstation"
    exit 1
fi