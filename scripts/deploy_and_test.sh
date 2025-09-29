#!/bin/bash

echo "=== Deploying Companion Self-Test to VM ==="

# Check if VM is accessible
if ! ssh vm 'echo "VM is ready"' 2>/dev/null; then
    echo "ERROR: Cannot connect to VM. Is it running?"
    echo "Start with: ./scripts/launch_qemu_membackend.sh"
    exit 1
fi

echo "VM is accessible"

# Copy companion source to VM
echo "Copying companion source to VM..."
scp -q src/companion_selftest.c vm:~/

# Compile on VM
echo "Compiling companion in VM..."
ssh vm 'gcc -o companion_selftest companion_selftest.c 2>&1 | grep -v "warning"'

# Run companion in background (it stays alive for 30 seconds)
echo "Starting companion in VM (will run for 30 seconds)..."
ssh vm './companion_selftest' &
COMPANION_PID=$!

# Give companion time to initialize
sleep 2

# Run Haywire scanner on host
echo ""
echo "=== Running Haywire Scanner on Host ==="
python3 test_beacon_selftest.py

# Wait for companion to finish
echo ""
echo "Waiting for companion to complete..."
wait $COMPANION_PID

echo ""
echo "=== Test Complete ==="