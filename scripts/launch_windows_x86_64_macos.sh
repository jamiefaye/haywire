#!/bin/bash

# Launch Windows 11 x86_64 VM on macOS
#
# On Intel Macs this uses HVF (Hypervisor.framework) with -cpu host.
# On Apple Silicon it falls back to TCG emulation (software emulation)
# because HVF cannot run x86_64 code there. Performance will be SLOW but
# functional.
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

# Intel hosts get hardware acceleration via HVF; Apple Silicon needs TCG.
# Check hw.optional.arm64 rather than uname -m, which says x86_64 under Rosetta.
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then
    ACCEL_NAME="TCG"
    ACCEL_ARGS=(-cpu max)
    DISPLAY_ARGS=(-vga std)
    echo "=== Windows 11 x86_64 on macOS (Apple Silicon) ==="
    echo ""
    echo "⚠️  WARNING: This uses TCG emulation (software)"
    echo "   Performance will be SLOW compared to native execution"
    echo "   Consider using a Linux host with KVM for better performance"
    echo ""
else
    ACCEL_NAME="HVF"
    # Not -cpu host: under QEMU 11.1 HVF, Windows 11 bugchecks with
    # IRQL_NOT_LESS_OR_EQUAL ~90s after reaching the lock screen (with or
    # without +invtsc, even with -smp 1), and a VT-x-visible CPU sends it to
    # Automatic Repair (see docs/windows11_vm_setup.md). A named model with
    # the hypervisor bit hidden ran clean in soak tests on this i9-9980HK.
    ACCEL_ARGS=(-accel hvf -cpu Skylake-Client-noTSX-IBRS,-hypervisor)
    # QEMU 11.1's x86 HVF asserts (hvf-all.c do_hv_vm_protect) when VGA dirty
    # tracking covers a framebuffer that isn't a whole number of 4K pages:
    # 800x600x32bpp is 0x1d4c00 bytes and crashes; 1280x800 is page-aligned.
    # Pin the mode via EDID. firmware/OVMF_CODE.fd (edk2-stable202011) ignores
    # EDID and switches to 800x600, so use the host QEMU's newer edk2, which
    # honors it. (ramfb and virtio-gpu-pci avoid the assert but stay blank once
    # x86 Windows boots without a PCI VGA device or the viogpudo driver.)
    DISPLAY_ARGS=(-vga none -device VGA,edid=on,xres=1280,yres=800)
    HVF_FIRMWARE=1
    echo "=== Windows 11 x86_64 on macOS (Intel, HVF) ==="
    echo ""
fi

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
if [ -n "$HVF_FIRMWARE" ]; then
    # See DISPLAY_ARGS above: HVF needs an edk2 that honors the VGA EDID mode
    for p in "$(brew --prefix 2>/dev/null)" /usr/local /opt/local; do
        if [ -n "$p" ] && [ -f "$p/share/qemu/edk2-x86_64-code.fd" ]; then
            OVMF_CODE="$p/share/qemu/edk2-x86_64-code.fd"
            break
        fi
    done
fi
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
if [ -n "$HVF_FIRMWARE" ]; then
    # Any TPM device (tpm-tis or tpm-crb) adds a 0x400-byte PPI RAM region at
    # 0xfed45000. HVF can't map sub-page RAM, so edk2 hangs at "[TPM2PP]" when
    # it touches it (and Windows reboots hang the same way). QEMU 11.1 has no
    # ppi=off, so run without a TPM; the swtpm state in $SWTPM_DIR is kept for
    # TCG hosts.
    echo "NOTE: TPM disabled under HVF (QEMU 11.1 PPI region hangs the firmware)"
    echo ""
    USE_TPM=0
elif ! command -v swtpm &> /dev/null; then
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

echo "Starting Windows 11 VM with $ACCEL_NAME..."
echo "Memory backend: $MEMFILE"
echo "Ports: QMP=$QMP_PORT, Monitor=$MONITOR_PORT"
echo ""
if [ "$ACCEL_NAME" = "TCG" ]; then
    echo "This will be SLOW. Be patient during boot."
    echo ""
fi

# Build TPM parameters if available
TPM_PARAMS=()
if [ "$USE_TPM" -eq 1 ]; then
    TPM_PARAMS=(
        -chardev "socket,id=chrtpm,path=$SWTPM_DIR/swtpm-sock"
        -tpmdev emulator,id=tpm0,chardev=chrtpm
        -device tpm-tis,tpmdev=tpm0
    )
fi

# Launch QEMU - match Linux script configuration
qemu-system-x86_64 \
    -machine q35 \
    "${ACCEL_ARGS[@]}" \
    -smp $CORES \
    -m $MEMORY \
    -rtc base=localtime \
    -object memory-backend-file,id=mem,size=$MEMORY,mem-path=$MEMFILE,share=on \
    -numa node,memdev=mem \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -device ahci,id=ahci \
    -drive file="$DISK_IMAGE",if=none,id=disk,format=qcow2 \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -boot order=c,menu=on \
    "${DISPLAY_ARGS[@]}" \
    -usb \
    -device usb-kbd \
    -device usb-tablet \
    "${TPM_PARAMS[@]}" \
    -qmp tcp:localhost:$QMP_PORT,server=on,wait=off \
    -monitor telnet:localhost:$MONITOR_PORT,server=on,wait=off \
    -name "Windows11-x86_64-$ACCEL_NAME"

# Clean up swtpm if it was started
if [ "$USE_TPM" -eq 1 ] && [ ! -z "$SWTPM_PID" ]; then
    echo "Stopping TPM emulator (PID: $SWTPM_PID)..."
    kill $SWTPM_PID 2>/dev/null || true
fi

echo ""
echo "VM shut down."
