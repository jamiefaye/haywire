#!/bin/bash

# Check if memory-backend file is actually changing

MEMFILE="/tmp/haywire-vm-mem"

if [ ! -f "$MEMFILE" ]; then
    echo "Memory backend file not found: $MEMFILE"
    exit 1
fi

echo "Monitoring $MEMFILE for changes..."
echo "Taking 5 samples, 1 second apart"
echo ""

# Take checksums at different offsets
for i in {1..5}; do
    echo "Sample $i:"
    
    # Check a few different regions
    for offset in 0 1048576 10485760 104857600; do
        # Read 1KB at each offset and checksum it
        checksum=$(dd if="$MEMFILE" bs=1024 count=1 skip=$((offset/1024)) 2>/dev/null | md5)
        echo "  Offset 0x$(printf %x $offset): $checksum"
    done
    
    echo ""
    sleep 1
done

echo "If checksums don't change, the memory-backend might be:"
echo "1. Not actually live (static snapshot)"
echo "2. Using MAP_PRIVATE instead of MAP_SHARED"
echo "3. Not the active guest memory"