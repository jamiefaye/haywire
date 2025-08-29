#!/bin/bash

# Quick setup script for Alpine Linux with VLC in QEMU
# This creates a minimal Alpine Linux environment with VLC for testing

ALPINE_VERSION="3.19"
ALPINE_ARCH="x86_64"
ALPINE_ISO="alpine-virt-${ALPINE_VERSION}.0-${ALPINE_ARCH}.iso"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/${ALPINE_ARCH}/${ALPINE_ISO}"

DISK_IMAGE="alpine_vlc.qcow2"
DISK_SIZE="4G"

echo "=== Alpine Linux + VLC QEMU Setup ==="
echo ""

# Download Alpine Linux if not present
if [ ! -f "$ALPINE_ISO" ]; then
    echo "Downloading Alpine Linux ${ALPINE_VERSION}..."
    curl -L -o "$ALPINE_ISO" "$ALPINE_URL"
    if [ $? -ne 0 ]; then
        echo "Failed to download Alpine Linux ISO"
        exit 1
    fi
else
    echo "Alpine ISO already present: $ALPINE_ISO"
fi

# Create disk image if not present
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE ($DISK_SIZE)"
    qemu-img create -f qcow2 "$DISK_IMAGE" "$DISK_SIZE"
else
    echo "Disk image already exists: $DISK_IMAGE"
fi

echo ""
echo "Setup complete! Now you can:"
echo ""
echo "1. Run the VM with: ./launch_alpine_vlc.sh"
echo "2. Install Alpine Linux to the disk"
echo "3. Install VLC with: apk add vlc xorg-server xf86-video-qxl"
echo ""
echo "Note: Alpine uses a minimal setup. You'll need to:"
echo "  - Run 'setup-alpine' after booting"
echo "  - Install X11 and VLC packages"
echo "  - Configure networking if needed"