#!/bin/bash

# Launch Windows 11 VM with memory-backend-file for Haywire introspection
#
# Requirements:
# - Windows 11 ISO (download from Microsoft)
# - OVMF UEFI firmware
# - swtpm for TPM 2.0 emulation (Windows 11 requirement)

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Configuration - use native WSL storage for better disk I/O performance
# (accessing /mnt/c/ via 9P is very slow)
WSL_VM_DIR="$HOME/haywire-vms"
DISK_IMAGE="$WSL_VM_DIR/windows11.qcow2"
DISK_SIZE="60G"  # Windows 11 needs at least 64GB
MEMORY="8G"      # Windows 11 needs at least 4GB, 8GB recommended
CORES="4"
QMP_PORT=4445
MONITOR_PORT=4444

# Memory backend for Haywire
MEMFILE="/tmp/haywire-vm-mem"

# ISO path - you need to download this
WIN11_ISO="$SCRIPT_DIR/../isos/Win11_25H2_English_x64.iso"

# VirtIO drivers ISO for Windows (optional but recommended for better performance)
VIRTIO_ISO="$SCRIPT_DIR/../isos/virtio-win.iso"

echo "=== Windows 11 VM with Haywire Support ==="
echo ""

# Check if ISO exists
if [ ! -f "$WIN11_ISO" ]; then
    echo "ERROR: Windows 11 ISO not found at: $WIN11_ISO"
    echo ""
    echo "Please download Windows 11 from:"
    echo "https://www.microsoft.com/software-download/windows11"
    echo ""
    echo "Save it to: $SCRIPT_DIR/../isos/Win11_24H2_English_x64.iso"
    echo "(or update the WIN11_ISO variable in this script)"
    exit 1
fi

# Determine qemu-img location
QEMU_IMG="/home/jamie/qemu-build/build-x86/qemu-img"
if [ ! -f "$QEMU_IMG" ]; then
    QEMU_IMG="qemu-img"
fi

# Create disk image if it doesn't exist
if [ ! -f "$DISK_IMAGE" ]; then
    echo "Creating disk image: $DISK_IMAGE ($DISK_SIZE)"
    mkdir -p "$(dirname "$DISK_IMAGE")"
    $QEMU_IMG create -f qcow2 "$DISK_IMAGE" "$DISK_SIZE"
    echo ""
fi

# Create ISO directory if needed
mkdir -p "$SCRIPT_DIR/../isos"

# Clean up any existing memory file
rm -f "$MEMFILE"

# Check for OVMF (UEFI firmware) - needed for Windows 11
OVMF_DIR="$SCRIPT_DIR/../firmware"
OVMF_CODE="$OVMF_DIR/OVMF_CODE.fd"
OVMF_VARS="$SCRIPT_DIR/../vms/windows11_VARS.fd"

# Download OVMF if not present
if [ ! -f "$OVMF_CODE" ] || [ ! -s "$OVMF_CODE" ]; then
    echo "Downloading OVMF UEFI firmware..."
    mkdir -p "$OVMF_DIR"
    # Download from Ubuntu's OVMF package on packages.ubuntu.com
    echo "Fetching OVMF from Ubuntu repositories..."
    wget -q -O "/tmp/ovmf.deb" "http://archive.ubuntu.com/ubuntu/pool/universe/e/edk2/ovmf_2023.05-2_all.deb" || {
        echo "ERROR: Failed to download OVMF package"
        echo "You can manually install with: sudo apt-get install ovmf"
        exit 1
    }
    # Extract the .deb file
    cd /tmp
    ar x ovmf.deb data.tar.xz
    tar -xf data.tar.xz ./usr/share/OVMF/OVMF_CODE.fd ./usr/share/OVMF/OVMF_VARS.fd 2>/dev/null
    mv ./usr/share/OVMF/OVMF_CODE.fd "$OVMF_CODE"
    mv ./usr/share/OVMF/OVMF_VARS.fd "$OVMF_DIR/OVMF_VARS.fd"
    rm -rf /tmp/ovmf.deb /tmp/data.tar.xz /tmp/usr
    cd - > /dev/null
    echo "OVMF firmware extracted successfully"
fi

# Create OVMF VARS file if it doesn't exist (stores UEFI variables)
if [ ! -f "$OVMF_VARS" ]; then
    echo "Creating OVMF VARS file..."
    mkdir -p "$(dirname "$OVMF_VARS")"
    OVMF_VARS_TEMPLATE="$OVMF_DIR/OVMF_VARS.fd"
    if [ -f "$OVMF_VARS_TEMPLATE" ]; then
        cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"
    else
        echo "ERROR: OVMF_VARS template not found"
        exit 1
    fi
fi

# Setup TPM 2.0 (required for Windows 11)
SWTPM_DIR="$SCRIPT_DIR/../vms/swtpm_win11"
mkdir -p "$SWTPM_DIR"

# Check if swtpm is installed
if ! command -v swtpm &> /dev/null; then
    echo "WARNING: swtpm not found (needed for TPM 2.0)"
    echo "Install with: sudo apt-get install swtpm swtpm-tools"
    echo ""
    echo "Continuing without TPM - Windows 11 installation may fail"
    echo "Press Ctrl+C to cancel, or wait 5 seconds to continue..."
    sleep 5
    USE_TPM=0
else
    USE_TPM=1
    # Start swtpm in background
    echo "Starting TPM 2.0 emulation..."
    swtpm socket --tpmstate dir="$SWTPM_DIR" \
        --ctrl type=unixio,path="$SWTPM_DIR/swtpm-sock" \
        --tpm2 \
        --log level=0 &
    SWTPM_PID=$!
    sleep 1
fi

echo "Memory backend: $MEMFILE"
echo "Disk: $DISK_IMAGE"
echo "RAM: $MEMORY"
echo "CPUs: $CORES"
echo ""
echo "QMP port: $QMP_PORT"
echo "Monitor port: $MONITOR_PORT"
echo ""

# Build QEMU command
QEMU_ARGS=(
    -machine q35
    -cpu host
    -smp $CORES
    -m $MEMORY

    # Memory backend for Haywire
    -object "memory-backend-file,id=mem,size=$MEMORY,mem-path=$MEMFILE,share=on"
    -numa "node,memdev=mem"

    # UEFI firmware
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS"

    # AHCI controller for both disk and CD-ROM
    -device ahci,id=ahci

    # Disk - SATA for Windows compatibility (no driver needed)
    -drive "file=$DISK_IMAGE,if=none,id=disk,format=qcow2"
    -device ide-hd,drive=disk,bus=ahci.0

    # Windows 11 ISO - SATA CD-ROM
    -drive "file=$WIN11_ISO,if=none,id=cdrom,media=cdrom,readonly=on"
    -device ide-cd,drive=cdrom,bus=ahci.1

    # Boot order - CD first for installation (d=cdrom, c=disk)
    -boot order=d,menu=on

    # Network - DISABLED for easier installation (not needed for Haywire)
    # -netdev user,id=net0
    # -device virtio-net-pci,netdev=net0

    # Graphics - GTK for native window display (via WSLg)
    # Alternatives: -display sdl, or -vnc :1 for VNC
    -vga std
    -display gtk

    # USB
    -usb
    -device usb-kbd
    -device usb-tablet

    # QMP and Monitor for Haywire
    -qmp "tcp:localhost:$QMP_PORT,server,nowait"
    -monitor "telnet:localhost:$MONITOR_PORT,server,nowait"

    -name "Windows-11-Haywire"
)

# Add TPM if available
if [ $USE_TPM -eq 1 ]; then
    QEMU_ARGS+=(
        -chardev "socket,id=chrtpm,path=$SWTPM_DIR/swtpm-sock"
        -tpmdev emulator,id=tpm0,chardev=chrtpm
        -device tpm-tis,tpmdev=tpm0
    )
fi

# Add VirtIO drivers ISO if available
if [ -f "$VIRTIO_ISO" ]; then
    echo "VirtIO drivers: $VIRTIO_ISO"
    QEMU_ARGS+=(-drive "file=$VIRTIO_ISO,media=cdrom,readonly=on")
fi

echo "Starting Windows 11 VM..."
echo ""

# Use system QEMU (has SDL/GTK display support)
# Custom build at /home/jamie/qemu-build/build-x86/ lacks GUI backends
QEMU_BIN="qemu-system-x86_64"
echo "Using system QEMU: $QEMU_BIN"

# Enable KVM if available (Linux only)
if [ -e /dev/kvm ]; then
    echo "KVM acceleration: enabled"
    QEMU_ARGS=(-enable-kvm "${QEMU_ARGS[@]}")
else
    echo "KVM acceleration: disabled (not available)"
fi

echo ""
echo "=== INSTALLATION NOTES ==="
echo "1. Windows 11 may require internet connection during setup"
echo "2. To bypass Microsoft account requirement:"
echo "   - Press Shift+F10 to open Command Prompt"
echo "   - Type: OOBE\\BYPASSNRO"
echo "   - System will reboot, then you can create local account"
echo "3. After installation, install guest tools for better performance"
echo ""
echo "Press Enter to start VM..."
read

$QEMU_BIN "${QEMU_ARGS[@]}"

# Cleanup
echo ""
echo "VM shut down."
echo "Cleaning up..."
rm -f "$MEMFILE"

if [ $USE_TPM -eq 1 ] && [ ! -z "$SWTPM_PID" ]; then
    kill $SWTPM_PID 2>/dev/null
fi

echo "Done."
