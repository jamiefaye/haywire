#!/bin/bash

# Launch TinyCore Linux - simpler than Alpine, GUI by default
# TinyCore is much easier for testing VLC

TINYCORE_ISO="TinyCore-current.iso"
TINYCORE_URL="http://tinycorelinux.net/14.x/x86/release/TinyCore-current.iso"
DISK_IMAGE="tinycore_vlc.qcow2"

# QEMU configuration
MEMORY="1G"
CORES="2"
QMP_PORT=4445
MONITOR_PORT=4444

echo "=== TinyCore Linux with VLC ==="

# Download TinyCore if not present (only 23MB!)
if [ ! -f "$TINYCORE_ISO" ]; then
    echo "Downloading TinyCore Linux (23MB)..."
    curl -L -o "$TINYCORE_ISO" "$TINYCORE_URL"
fi

# Create disk image if not present
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE"
    qemu-img create -f qcow2 "$DISK_IMAGE" 2G
fi

echo "Starting QEMU..."
echo "QMP Port: $QMP_PORT (for Haywire)"
echo "Monitor Port: $MONITOR_PORT"
echo ""
echo "After boot:"
echo "1. Click Apps icon"
echo "2. Search for 'vlc'"
echo "3. Click Download + Load"
echo ""

# Launch QEMU with TinyCore
qemu-system-x86_64 \
    -m $MEMORY \
    -smp $CORES \
    -cdrom "$TINYCORE_ISO" \
    -drive file="$DISK_IMAGE",format=qcow2,if=virtio \
    -boot d \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -device e1000,netdev=net0 \
    -netdev user,id=net0 \
    -vga std \
    -usb -device usb-tablet \
    -display default,show-cursor=on \
    -name "TinyCore-VLC"

echo "VM shut down."