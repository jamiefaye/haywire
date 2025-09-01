#!/bin/bash
# Build QEMU with Haywire VA->PA translation support

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
QEMU_DIR="$SCRIPT_DIR/qemu-src"
QEMU_REPO="https://gitlab.com/qemu-project/qemu.git"
QEMU_VERSION="v9.1.0"  # Stable version we tested with

echo "=== QEMU Builder for Haywire ==="

# Clone QEMU if not exists
if [ ! -d "$QEMU_DIR" ]; then
    echo "Cloning QEMU $QEMU_VERSION..."
    git clone --depth 1 --branch $QEMU_VERSION $QEMU_REPO "$QEMU_DIR"
else
    echo "QEMU source already exists"
fi

cd "$QEMU_DIR"

# Apply our patch
echo "Applying Haywire VA->PA patch..."
if git apply --check "$SCRIPT_DIR/qemu-va2pa.patch" 2>/dev/null; then
    git apply "$SCRIPT_DIR/qemu-va2pa.patch"
    echo "Patch applied successfully"
else
    echo "Patch already applied or conflicts exist"
fi

# Configure for ARM64 only (faster build)
echo "Configuring QEMU..."
./configure --target-list=aarch64-softmmu --enable-debug

# Build
echo "Building QEMU (this will take a while)..."
ninja -C build -j$(sysctl -n hw.ncpu)

# Sign for macOS
echo "Signing QEMU binary..."
codesign --entitlements accel/hvf/entitlements.plist --force -s - build/qemu-system-aarch64-unsigned

# Copy to our directory
echo "Installing QEMU binary..."
cp build/qemu-system-aarch64-unsigned "$SCRIPT_DIR/"

echo "=== Build complete! ==="
echo "Binary installed at: $SCRIPT_DIR/qemu-system-aarch64-unsigned"