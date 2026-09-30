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

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Launch QEMU with memory backend
# Use our custom build if it exists, otherwise use system QEMU
QEMU_BIN="$SCRIPT_DIR/../qemu-mods/qemu-src/build/qemu-system-aarch64-unsigned"
if [ ! -f "$QEMU_BIN" ]; then
    # Try the old location
    QEMU_BIN="$SCRIPT_DIR/../qemu-mods/qemu-system-aarch64-unsigned"
    if [ ! -f "$QEMU_BIN" ]; then
        QEMU_BIN="qemu-system-aarch64"
    fi
    echo "Using system QEMU (custom build not found)"
else
    echo "Using custom QEMU build with VA->PA translation support"
fi

# UEFI firmware ships with qemu: Homebrew's is under /opt/homebrew on Apple
# Silicon, /usr/local on Intel; MacPorts' (used on Intel, where Homebrew no
# longer builds qemu) is under /opt/local
BREW_PREFIX="$(brew --prefix 2>/dev/null)"
for p in "$BREW_PREFIX" /opt/homebrew /usr/local /opt/local; do
    if [ -n "$p" ] && [ -f "$p/share/qemu/edk2-aarch64-code.fd" ]; then
        BIOS="$p/share/qemu/edk2-aarch64-code.fd"
        break
    fi
done
if [ -z "$BIOS" ]; then
    echo "ERROR: edk2-aarch64-code.fd not found (install qemu via Homebrew, or MacPorts on Intel)"
    exit 1
fi

# HVF can only run an ARM64 guest on an ARM64 host; Intel Macs need TCG.
# Check hw.optional.arm64 rather than uname -m, which says x86_64 under Rosetta.
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then
    ACCEL_ARGS=(-accel hvf -cpu host)
else
    echo "Intel host: using TCG emulation (slow)"
    ACCEL_ARGS=(-accel tcg,thread=multi -cpu max)
fi

$QEMU_BIN \
    -M virt,highmem=on \
    "${ACCEL_ARGS[@]}" \
    -m $MEMSIZE \
    -object memory-backend-file,id=mem,size=$MEMSIZE,mem-path=$MEMFILE,share=on,prealloc=on \
    -numa node,memdev=mem \
    -smp 4 \
    -bios "$BIOS" \
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
    -name "Ubuntu-ARM64-MemBackend"

# Clean up memory file on exit
echo "Cleaning up memory backend file..."
rm -f "$MEMFILE"