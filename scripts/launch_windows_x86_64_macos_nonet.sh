#!/bin/bash

# Launch Windows 11 x86_64 VM on macOS (Apple Silicon) - NO NETWORK
#
# NOTE: This uses TCG emulation (software emulation) because Apple Silicon
# cannot natively run x86_64 code. Performance will be SLOW but functional.
#
# NETWORK IS DISABLED - VM cannot access internet or receive updates
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

echo "=== Windows 11 x86_64 on macOS (Apple Silicon) - NO NETWORK ==="
echo ""
echo "🚫 NETWORK DISABLED - VM is isolated from internet"
echo "   No updates, no downloads, no network connectivity"
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

# OVMF (UEFI firmware) paths - use Linux firmware for compatibility
OVMF_CODE="$SCRIPT_DIR/../firmware/OVMF_CODE.fd"
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

# Copy VARS template if it doesn't exist
OVMF_VARS_TEMPLATE="$SCRIPT_DIR/../firmware/OVMF_VARS.fd"
if [ ! -f "$OVMF_VARS" ]; then
    echo "Creating OVMF VARS file from Linux firmware template..."
    mkdir -p "$(dirname "$OVMF_VARS")"
    if [ -f "$OVMF_VARS_TEMPLATE" ]; then
        cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"
        echo "OVMF VARS file created (CODE: 3.5MB + VARS: 528KB = ~4MB, well under 8MB limit)"
    else
        echo "ERROR: OVMF_VARS template not found at: $OVMF_VARS_TEMPLATE"
        echo "Make sure firmware files from Linux setup are in firmware/ directory"
        exit 1
    fi
fi

# Setup TPM 2.0 (required for Windows 11)
SWTPM_DIR="$SCRIPT_DIR/../vms/swtpm_win11"
mkdir -p "$SWTPM_DIR"

# Check if swtpm is installed
if ! command -v swtpm &> /dev/null; then
    echo "WARNING: swtpm not found (needed for TPM 2.0)"
    echo "Install with: brew install swtpm"
    echo ""
    echo "Continuing without TPM - Windows 11 may not boot properly"
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
    echo "TPM 2.0 emulator started (PID: $SWTPM_PID)"
fi

echo "Starting Windows 11 VM with TCG emulation..."
echo "Memory backend: $MEMFILE"
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT"
echo ""
echo "This will be SLOW. Be patient during boot."
echo ""

# Build TPM parameters if available
TPM_PARAMS=()
if [ "$USE_TPM" -eq 1 ]; then
    TPM_PARAMS=(
        -chardev "socket,id=chrtpm,path=$SWTPM_DIR/swtpm-sock"
        -tpmdev emulator,id=tpm0,chardev=chrtpm
        -device tpm-tis,tpmdev=tpm0
    )
fi

# Launch QEMU with TCG emulation - match Linux script configuration
# Note: No -accel flag = defaults to TCG on Apple Silicon
qemu-system-x86_64 \
    -machine q35 \
    -cpu max \
    -smp $CORES \
    -m $MEMORY \
    -object memory-backend-file,id=mem,size=$MEMORY,mem-path=$MEMFILE,share=on \
    -numa node,memdev=mem \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -device ahci,id=ahci \
    -drive file="$DISK_IMAGE",if=none,id=disk,format=qcow2 \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -boot order=c,menu=on \
    -vga std \
    -usb \
    -device usb-kbd \
    -device usb-tablet \
    "${TPM_PARAMS[@]}" \
    -nic none \
    -qmp tcp:localhost:$QMP_PORT,server=on,wait=off \
    -monitor telnet:localhost:$MONITOR_PORT,server=on,wait=off \
    -name "Windows11-x86_64-TCG-NoNet"

# Clean up swtpm if it was started
if [ "$USE_TPM" -eq 1 ] && [ ! -z "$SWTPM_PID" ]; then
    echo "Stopping TPM emulator (PID: $SWTPM_PID)..."
    kill $SWTPM_PID 2>/dev/null || true
fi

echo ""
echo "VM shut down."
