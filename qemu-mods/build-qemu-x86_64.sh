#!/bin/bash
# Build QEMU x86_64 for Windows VM support
# First build: Vanilla (no patches)
# Second build: With kernel-memory-expose patch

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
QEMU_DIR="$SCRIPT_DIR/qemu-src"

echo "========================================"
echo "QEMU x86_64 Builder for Haywire"
echo "========================================"
echo ""

if [ ! -d "$QEMU_DIR" ]; then
    echo "ERROR: QEMU source not found at $QEMU_DIR"
    echo "Run build-qemu.sh first to clone QEMU source"
    exit 1
fi

cd "$QEMU_DIR"

echo "Building VANILLA QEMU x86_64 (no patches)"
echo "This verifies the build toolchain works..."
echo ""

# Clean previous build
echo "[1/4] Cleaning previous build..."
rm -rf build-x86
mkdir -p build-x86
cd build-x86

# Configure for x86_64 system emulation
echo "[2/4] Configuring QEMU for x86_64..."
echo "  Target: x86_64-softmmu (Windows/Linux VMs)"
echo "  Debug: Enabled"
echo ""

../configure \
    --target-list=x86_64-softmmu \
    --enable-debug \
    --disable-werror

# Build
echo ""
echo "[3/4] Building QEMU..."
echo "  This will take 5-10 minutes on first build"
echo "  Using all available CPU cores"
echo ""

# Detect number of CPUs
if command -v nproc &> /dev/null; then
    NCPU=$(nproc)
elif [ -f /proc/cpuinfo ]; then
    NCPU=$(grep -c processor /proc/cpuinfo)
else
    NCPU=4
fi

make -j${NCPU}

echo ""
echo "[4/4] Build complete!"
echo ""
echo "========================================"
echo "SUCCESS: Vanilla QEMU x86_64 built"
echo "========================================"
echo ""
echo "Binary: $QEMU_DIR/build-x86/qemu-system-x86_64.exe"
echo ""
echo "Test it with:"
echo "  $QEMU_DIR/build-x86/qemu-system-x86_64.exe --version"
echo ""
echo "Next step: Apply kernel-memory-expose patch and rebuild"
