#!/bin/bash

# Simplest approach - use Debian Live with XFCE (has package manager working out of box)

DEBIAN_ISO="debian-live.iso"
DEBIAN_URL="https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-12.7.0-amd64-xfce.iso"

# QEMU configuration  
MEMORY="2G"
CORES="2"
QMP_PORT=4445
MONITOR_PORT=4444

echo "=== Simple VLC Test Environment ==="
echo ""
echo "This uses Debian Live XFCE - everything works out of the box"
echo ""

# Check if ISO exists
if [ ! -f "$DEBIAN_ISO" ]; then
    echo "You need to download Debian Live XFCE first:"
    echo ""
    echo "Download from:"
    echo "$DEBIAN_URL"
    echo ""
    echo "Then save as: $DEBIAN_ISO"
    echo ""
    echo "Or use wget:"
    echo "wget -O $DEBIAN_ISO '$DEBIAN_URL'"
    exit 1
fi

echo "Starting QEMU..."
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT"
echo ""
echo "Once booted:"
echo "1. Open Terminal"  
echo "2. Run: sudo apt update && sudo apt install vlc -y"
echo "3. Run: vlc"
echo ""

# Simple QEMU launch
qemu-system-x86_64 \
    -m $MEMORY \
    -smp $CORES \
    -cdrom "$DEBIAN_ISO" \
    -boot d \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -vga std \
    -usb -device usb-tablet \
    -display default,show-cursor=on

echo "VM shut down."