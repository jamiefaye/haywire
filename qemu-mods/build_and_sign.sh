#!/bin/bash

# Build and sign QEMU with HVF support for macOS

set -e

echo "=== Building QEMU with introspection support ==="

cd /Users/jamie/haywire/qemu-mods/qemu-src

# Build QEMU
echo "Building QEMU..."
make -j8

# Check if build succeeded
if [ ! -f "build/qemu-system-aarch64-unsigned" ]; then
    echo "Build failed - binary not found"
    exit 1
fi

# Copy to deployment location
echo "Copying binary..."
cp build/qemu-system-aarch64-unsigned ../qemu-system-aarch64-unsigned

# Create entitlements if needed
if [ ! -f "../hvf.entitlements" ]; then
    echo "Creating entitlements file..."
    cat > ../hvf.entitlements << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.hypervisor</key>
    <true/>
</dict>
</plist>
EOF
fi

# Sign with HVF entitlement
echo "Signing binary with HVF entitlement..."
codesign --entitlements ../hvf.entitlements --force -s - ../qemu-system-aarch64-unsigned

echo "=== Build complete ==="
echo "Binary location: /Users/jamie/haywire/qemu-mods/qemu-system-aarch64-unsigned"
echo "Signed with HVF support: YES"

# Show binary info
ls -lh ../qemu-system-aarch64-unsigned