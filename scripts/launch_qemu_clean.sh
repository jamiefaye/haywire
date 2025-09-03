#!/bin/bash

# Clean QEMU launch script - no external hacking
# Ready for cloud-init injection

echo "Starting QEMU with cloud-init support..."
echo "Memory will be at: /tmp/haywire-vm-mem for shared memory communication"
echo ""

# Clean up any existing memory file
MEMFILE="/tmp/haywire-vm-mem"
MEMSIZE="4G"
rm -f "$MEMFILE"

# Get script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Use our built QEMU or fall back to system
QEMU_BIN="$SCRIPT_DIR/../qemu-mods/qemu-system-aarch64-unsigned"
if [ ! -f "$QEMU_BIN" ]; then
    QEMU_BIN="qemu-system-aarch64"
    echo "Using system QEMU"
else
    echo "Using custom-built QEMU (now clean, no Haywire)"
fi

# Check if seed.iso exists for cloud-init
CLOUD_INIT=""
if [ -f "/tmp/seed.iso" ]; then
    echo "Found cloud-init seed.iso - will auto-inject code!"
    CLOUD_INIT="-drive file=/tmp/seed.iso,if=virtio,format=raw"
else
    echo "No seed.iso found - VM will boot normally"
    echo "To inject code, create seed.iso with cloud-init data"
fi

$QEMU_BIN \
    -M virt,highmem=on \
    -accel hvf \
    -cpu host \
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
    $CLOUD_INIT \
    -boot c \
    -qmp tcp:localhost:4445,server=on,wait=off \
    -monitor telnet:localhost:4444,server,wait=off \
    -chardev socket,path=/tmp/qga.sock,server=on,wait=off,id=qga0 \
    -device virtio-serial \
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -serial stdio \
    -name "Ubuntu-ARM64-Clean"

echo "Cleaning up..."
rm -f "$MEMFILE"