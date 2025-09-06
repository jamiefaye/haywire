#!/bin/bash

echo "=== Deploying Companion v2 to VM ==="

# Check if VM is accessible
if ! ssh vm 'echo "VM is ready"' 2>/dev/null; then
    echo "ERROR: Cannot connect to VM. Is it running?"
    echo "Start with: ./scripts/launch_qemu_membackend.sh"
    exit 1
fi

echo "VM is accessible"

# Copy companion source to VM
echo "Copying companion v2 source to VM..."
scp -q src/companion_v2.c vm:~/

# Compile on VM
echo "Compiling companion in VM..."
ssh vm 'gcc -o companion_v2 companion_v2.c 2>&1 | grep -v "warning"'

# Run companion in background on VM
echo "Starting companion in VM..."
ssh vm 'killall companion_v2 2>/dev/null; ./companion_v2' &
COMPANION_PID=$!

# Give companion time to initialize
sleep 2

# Run Haywire scanner on host
echo ""
echo "=== Running Haywire Scanner on Host ==="
echo "Scanning memory-backend-file for beacons..."
python3 scan_memfile_v2.py

# Keep companion running
echo ""
echo "Companion running in VM (press Ctrl+C to stop)"
wait $COMPANION_PID