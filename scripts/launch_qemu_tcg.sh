#!/bin/bash

# Launch QEMU with TCG (Tiny Code Generator) instead of HVF
# TCG is slower but gives us full control over the guest

echo "Starting QEMU with TCG (no HVF) for Haywire testing..."
echo "WARNING: This will be MUCH slower than HVF!"
echo ""

# Create memory backend file in RAM (tmpfs)
MEMFILE="/tmp/haywire-vm-mem"
MEMSIZE="4G"

# Clean up any existing memory file
rm -f "$MEMFILE"

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Launch QEMU with memory backend
# Use our custom build if it exists, otherwise use system QEMU
QEMU_BIN="$SCRIPT_DIR/../qemu-mods/qemu-system-aarch64-unsigned"
if [ ! -f "$QEMU_BIN" ]; then
    QEMU_BIN="qemu-system-aarch64"
    echo "Using system QEMU (custom build not found)"
else
    echo "Using custom QEMU build with Haywire support"
fi

echo "Starting with TCG (software emulation) - NO HVF!"
echo ""

$QEMU_BIN \
    -M virt,highmem=on \
    -cpu cortex-a72 \
    -m $MEMSIZE \
    -object memory-backend-file,id=mem,size=$MEMSIZE,mem-path=$MEMFILE,share=on,prealloc=on \
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
    -drive if=virtio,format=qcow2,file="$SCRIPT_DIR/../vms/ubuntu_arm64.qcow2" \
    -boot c \
    -qmp tcp:localhost:4445,server=on,wait=off \
    -monitor telnet:localhost:4444,server=on,wait=off \
    -gdb tcp::1234 \
    -chardev socket,path=/tmp/qga.sock,server=on,wait=off,id=qga0 \
    -device virtio-serial \
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -monitor stdio \
    -name "Ubuntu-ARM64-TCG-NoHVF"

# Clean up memory file on exit
echo "Cleaning up memory backend file..."
rm -f "$MEMFILE"