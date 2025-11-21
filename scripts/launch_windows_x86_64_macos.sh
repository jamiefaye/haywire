#!/bin/bash

# Launch Windows 11 x86_64 VM on macOS (Apple Silicon)
#
# NOTE: This uses TCG emulation (software emulation) because Apple Silicon
# cannot natively run x86_64 code. Performance will be SLOW but functional.
#
# Requirements:
# - QEMU (install via: brew install qemu)
# - Windows 11 disk image (already exists)
# - OVMF UEFI firmware (automatically downloaded if needed)

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Configuration
DISK_IMAGE="$SCRIPT_DIR/../vms/windows11.qcow2"
MEMORY="8G"      # Windows 11 needs at least 4GB, 8GB recommended
CORES="4"
QMP_PORT=4445
MONITOR_PORT=4444

# Memory backend for Haywire
MEMFILE="/tmp/haywire-vm-mem"

echo "=== Windows 11 x86_64 on macOS (Apple Silicon) ==="
echo ""
echo "⚠️  WARNING: This uses TCG emulation (software)"
echo "   Performance will be SLOW compared to native execution"
echo "   Consider using a Linux host with KVM for better performance"
echo ""

# Check if disk exists
if [ ! -f "$DISK_IMAGE" ]; then
    echo "ERROR: Windows 11 disk image not found at: $DISK_IMAGE"
    echo ""
    echo "Expected file: $DISK_IMAGE"
    echo "This disk should already be installed and configured."
    exit 1
fi

# Check if QEMU is installed
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "ERROR: qemu-system-x86_64 not found"
    echo ""
    echo "Install QEMU with Homebrew:"
    echo "  brew install qemu"
    exit 1
fi

# Clean up any existing memory file
rm -f "$MEMFILE"

# OVMF (UEFI firmware) paths for macOS
OVMF_CODE="/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
OVMF_VARS="$SCRIPT_DIR/../vms/windows11_VARS.fd"

# Check if OVMF firmware exists
if [ ! -f "$OVMF_CODE" ]; then
    echo "ERROR: UEFI firmware not found at: $OVMF_CODE"
    echo ""
    echo "QEMU's UEFI firmware should be installed with:"
    echo "  brew install qemu"
    echo ""
    echo "If installed, firmware might be at a different location."
    echo "Common locations:"
    echo "  /opt/homebrew/share/qemu/edk2-x86_64-code.fd (Apple Silicon)"
    echo "  /usr/local/share/qemu/edk2-x86_64-code.fd (Intel Mac)"
    exit 1
fi

# Create OVMF VARS file if it doesn't exist (stores UEFI variables)
if [ ! -f "$OVMF_VARS" ]; then
    echo "Creating OVMF VARS file for UEFI variables..."
    mkdir -p "$(dirname "$OVMF_VARS")"
    # Try to copy template
    OVMF_VARS_TEMPLATE="/opt/homebrew/share/qemu/edk2-x86_64-vars.fd"
    if [ -f "$OVMF_VARS_TEMPLATE" ]; then
        cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"
    else
        # Create empty VARS file (QEMU will initialize it)
        dd if=/dev/zero of="$OVMF_VARS" bs=1M count=64 2>/dev/null
    fi
    echo "OVMF VARS file created"
fi

echo "Starting Windows 11 VM with TCG emulation..."
echo "Memory backend: $MEMFILE"
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT"
echo ""
echo "This will be SLOW. Be patient during boot."
echo ""

# Launch QEMU with TCG emulation
# Note: No -accel flag = defaults to TCG on Apple Silicon
qemu-system-x86_64 \
    -M q35 \
    -cpu qemu64 \
    -m $MEMORY \
    -smp $CORES \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -device virtio-vga \
    -display default,show-cursor=on \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -device intel-hda \
    -device hda-duplex \
    -drive if=virtio,format=qcow2,file="$DISK_IMAGE" \
    -object memory-backend-file,id=mem,size=$MEMORY,mem-path=$MEMFILE,share=on,prealloc=on \
    -numa node,memdev=mem \
    -qmp tcp:localhost:$QMP_PORT,server=on,wait=off \
    -monitor telnet:localhost:$MONITOR_PORT,server=on,wait=off \
    -netdev user,id=net0,hostfwd=tcp::3389-:3389 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -name "Windows11-x86_64-TCG" \
    -serial stdio

echo ""
echo "VM shut down."
