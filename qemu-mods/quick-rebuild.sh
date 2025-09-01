#!/bin/bash
# Quick rebuild after making changes - NO reconfigure, just rebuild

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
QEMU_DIR="$SCRIPT_DIR/qemu-src"

# Check if submodule is initialized
if [ ! -f "$QEMU_DIR/configure" ]; then
    echo "Initializing QEMU submodule..."
    cd "$SCRIPT_DIR/.."
    git submodule update --init --recursive
    cd "$QEMU_DIR"
    
    # Apply our patch
    echo "Applying Haywire patch..."
    git apply "$SCRIPT_DIR/qemu-va2pa.patch"
    
    # Configure
    echo "Configuring QEMU..."
    ./configure --target-list=aarch64-softmmu --enable-debug
fi

if [ ! -d "$QEMU_DIR/build" ]; then
    echo "No build directory found. Configuring..."
    cd "$QEMU_DIR"
    ./configure --target-list=aarch64-softmmu --enable-debug
fi

echo "=== Quick rebuild (incremental) ==="
cd "$QEMU_DIR"

# Just rebuild what changed (FAST!)
echo "Building changes..."
ninja -C build -j$(sysctl -n hw.ncpu)

# Re-sign
echo "Signing..."
codesign --entitlements accel/hvf/entitlements.plist --force -s - build/qemu-system-aarch64-unsigned

# Copy to our directory
cp build/qemu-system-aarch64-unsigned "$SCRIPT_DIR/"

echo "=== Quick rebuild complete! ==="
echo "Restart your VM to test changes"