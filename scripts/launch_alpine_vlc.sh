#!/bin/bash

# Launch Alpine Linux with VLC in QEMU for Haywire testing

ALPINE_ISO="alpine-virt-3.19.0-x86_64.iso"
DISK_IMAGE="alpine_vlc.qcow2"

# QEMU configuration
MEMORY="2G"
CORES="2"
QMP_PORT=4445
MONITOR_PORT=4444

# Check for macOS and use appropriate acceleration
if [[ "$OSTYPE" == "darwin"* ]]; then
    ACCEL=""  # No acceleration, will use TCG (software emulation)
    DISPLAY="-display default,show-cursor=on"
else
    ACCEL="-enable-kvm"
    DISPLAY="-display gtk"
fi

echo "=== Launching Alpine Linux with VLC ==="
echo "QMP Port: $QMP_PORT (for Haywire connection)"
echo "Monitor Port: $MONITOR_PORT"
echo ""

# Check if files exist
if [ ! -f "$DISK_IMAGE" ]; then
    echo "ERROR: Disk image not found: $DISK_IMAGE"
    echo "Run ./setup_alpine_vlc.sh first"
    exit 1
fi

# Determine boot device
BOOT_OPTS=""
if [ -f "$ALPINE_ISO" ] && [ ! -f ".alpine_installed" ]; then
    echo "Booting from ISO (first boot - install Alpine)"
    BOOT_OPTS="-cdrom $ALPINE_ISO -boot d"
else
    echo "Booting from disk"
    BOOT_OPTS="-boot c"
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
    -vga std \
    -device AC97 \
    -usb -device usb-tablet \
    $DISPLAY \
    -name "Alpine-VLC-Haywire"

echo ""
echo "VM has been shut down."