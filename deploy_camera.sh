#!/bin/bash

echo "=== Deploying Companion Camera to VM ==="

# Check if VM is accessible
if ! ssh vm 'echo "VM is ready"' 2>/dev/null; then
    echo "ERROR: Cannot connect to VM. Is it running?"
    echo "Start with: ./scripts/launch_qemu_membackend.sh"
    exit 1
fi

echo "VM is accessible"

# Copy companion source and header to VM
echo "Copying companion camera source to VM..."
scp -q src/companion_camera.c vm:~/
scp -q include/beacon_protocol.h vm:~/

# Compile on VM
echo "Compiling companion in VM..."
ssh vm 'gcc -I. -o companion_camera companion_camera.c 2>&1 | grep -v "warning"'

# Kill any existing companion
ssh vm 'killall companion_camera 2>/dev/null || true'

# Run companion in background on VM
echo "Starting companion camera in VM..."
ssh vm './companion_camera' &
COMPANION_PID=$!

# Give companion time to initialize
sleep 2

# Run Haywire scanner on host
echo ""
echo "=== Running Beacon Scanner on Host ==="
./test_beacon_scan

# Keep companion running for a bit more
echo ""
echo "Letting companion run for 10 seconds..."
sleep 10

# Kill companion
echo "Stopping companion..."
ssh vm 'killall companion_camera 2>/dev/null || true'

echo ""
echo "=== Test Complete ==="
