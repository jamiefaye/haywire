#!/bin/bash

# Launch x86_64 Ubuntu on Linux with KVM acceleration
# Perfect for WSL2 on Windows with nested virtualization enabled!

# Use absolute paths instead of $HOME (WSL may set HOME to Windows path)
HOME_DIR=$(cd ~ && pwd)
UBUNTU_ISO="$HOME_DIR/haywire/vms/ubuntu-22.04-desktop-amd64.iso"
DISK_IMAGE="$HOME_DIR/haywire/vms/ubuntu_x86_64.qcow2"
QEMU_BUILD="$HOME_DIR/haywire/qemu-mods/qemu-src/build"

# QEMU configuration
MEMORY="4G"
CORES="4"
QMP_PORT=4445
MONITOR_PORT=4444

echo "=== x86_64 Ubuntu with KVM Acceleration ==="
echo "Optimized for WSL2 + nested virtualization"
echo ""

# Create vms directory if needed
mkdir -p "$(dirname "$DISK_IMAGE")"

# Create disk if needed
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE (40G)"
    # Try custom qemu-img first, then system
    if [ -f "$QEMU_BUILD/qemu-img" ]; then
        "$QEMU_BUILD/qemu-img" create -f qcow2 "$DISK_IMAGE" 40G
    else
        qemu-img create -f qcow2 "$DISK_IMAGE" 40G
    fi
    FIRST_BOOT=1
fi

# Check if this is first boot or existing installation
if [ -n "$FIRST_BOOT" ]; then
    echo ""
    echo "=== FIRST BOOT DETECTED ==="
    echo "Will boot from ISO to install Ubuntu"
    echo ""
    if [ ! -f "$UBUNTU_ISO" ]; then
        echo "ERROR: Ubuntu ISO not found at: $UBUNTU_ISO"
        echo ""
        echo "Download from:"
        echo "Desktop: https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso"
        echo "Server:  https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso"
        echo ""
        echo "Save to: $UBUNTU_ISO"
        exit 1
    fi
    BOOT_ORDER="-boot d"
    CDROM_OPTION="-cdrom $UBUNTU_ISO"
else
    echo "Booting from existing installation..."
    BOOT_ORDER="-boot c"
    CDROM_OPTION=""
fi

echo "Starting QEMU with KVM acceleration..."
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT"
echo ""

# Find QEMU binary - prefer modified version, fall back to system
if [ -f "$QEMU_BUILD/qemu-system-x86_64" ]; then
    QEMU="$QEMU_BUILD/qemu-system-x86_64"
    echo "Using modified QEMU: $QEMU"
elif command -v qemu-system-x86_64 &> /dev/null; then
    QEMU="qemu-system-x86_64"
    echo "WARNING: Using system QEMU - custom QMP commands may not work!"
    echo "Build modified QEMU for full Haywire functionality"
else
    echo "ERROR: qemu-system-x86_64 not found!"
    echo "Install with: sudo apt-get install qemu-system-x86"
    exit 1
fi

# Check KVM availability
if [ ! -w /dev/kvm ]; then
    echo "WARNING: /dev/kvm not accessible!"
    echo "KVM acceleration will not work"
    echo ""
    echo "Fix with:"
    echo "  sudo usermod -a -G kvm $USER"
    echo "  # Then logout and login"
    echo ""
    read -p "Continue without KVM? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
    ACCEL_OPTION="-accel tcg"
else
    ACCEL_OPTION="-accel kvm"
fi

# Find UEFI firmware (OVMF for x86_64)
UEFI_CODE="/usr/share/OVMF/OVMF_CODE.fd"
if [ ! -f "$UEFI_CODE" ]; then
    UEFI_CODE="/usr/share/qemu/OVMF_CODE.fd"
fi
if [ ! -f "$UEFI_CODE" ]; then
    echo "WARNING: UEFI firmware not found!"
    echo "Install with: sudo apt-get install ovmf"
    echo "Will use legacy BIOS instead"
    BIOS_OPTION=""
else
    echo "Using UEFI firmware: $UEFI_CODE"
    BIOS_OPTION="-bios $UEFI_CODE"
fi

# Launch VM
$QEMU \
    -machine q35 \
    $ACCEL_OPTION \
    -cpu host \
    -m $MEMORY \
    -object memory-backend-file,id=mem,size=$MEMORY,mem-path=/tmp/haywire-vm-mem,share=on,prealloc=on \
    -numa node,memdev=mem \
    -smp $CORES \
    $BIOS_OPTION \
    -device VGA \
    -vnc :0 \
    -device qemu-xhci \
    -device usb-kbd \
    -device usb-tablet \
    -device intel-hda \
    -device hda-duplex \
    -drive if=virtio,format=qcow2,file="$DISK_IMAGE" \
    $CDROM_OPTION \
    $BOOT_ORDER \
    -qmp tcp:localhost:$QMP_PORT,server,nowait \
    -monitor telnet:localhost:$MONITOR_PORT,server,nowait \
    -chardev socket,path=/tmp/qga.sock,server=on,wait=off,id=qga0 \
    -device virtio-serial \
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -serial stdio \
    -name "Ubuntu-x86_64-KVM"

echo ""
echo "VM shut down."

if [ -n "$FIRST_BOOT" ]; then
    echo ""
    echo "=== INSTALLATION COMPLETE ==="
    echo "Run this script again to boot from the installed system"
fi
