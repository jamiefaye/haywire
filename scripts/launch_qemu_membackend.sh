#!/bin/bash

# Launch QEMU with memory-backend-file for direct memory access
# This provides zero-copy memory access for Haywire

echo "Starting QEMU with memory-backend-file for Haywire..."
echo "Memory will be exposed at: /dev/shm/haywire-vm-mem"
echo ""

# Create memory backend file in RAM (tmpfs)
MEMFILE="/dev/shm/haywire-vm-mem"
MEMSIZE="4G"

# For macOS, /dev/shm doesn't exist, use /tmp instead
if [[ "$OSTYPE" == "darwin"* ]]; then
    MEMFILE="/tmp/haywire-vm-mem"
    echo "Note: Using /tmp for memory backend (macOS)"
fi

# Clean up any existing memory file
rm -f "$MEMFILE"

# Launch QEMU with memory backend
qemu-system-aarch64 \
    -M virt,highmem=on \
    -accel hvf \
    -cpu host \
    -m $MEMSIZE \
    -object memory-backend-file,id=mem,size=$MEMSIZE,mem-path=$MEMFILE,share=on \
    -numa node,memdev=mem \
    -smp 4 \
    -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
    -device virtio-gpu-pci \
    -display default,show-cursor=on \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -device intel-hda \
    -device hda-duplex \
    -drive if=virtio,format=qcow2,file=ubuntu_arm64.qcow2 \
    -cdrom /Users/jamie/haywire/ubuntu-25.04-desktop-arm64.iso \
    -boot d \
    -qmp tcp:localhost:4445,server,nowait \
    -monitor telnet:localhost:4444,server,nowait \
    -gdb tcp::1234 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -serial stdio \
    -name "Ubuntu-ARM64-MemBackend"

# Clean up memory file on exit
echo "Cleaning up memory backend file..."
rm -f "$MEMFILE"