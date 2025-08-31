#!/usr/bin/env python3
"""
Test if memory changes at different locations
"""

import mmap
import time
import hashlib

filepath = "/tmp/haywire-vm-mem"

with open(filepath, "r+b") as f:
    mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    
    print("Testing different memory regions for changes...")
    print("Checking every 16MB through the first 1GB\n")
    
    # Store initial checksums
    checksums = {}
    for offset in range(0, 1024*1024*1024, 16*1024*1024):  # Every 16MB up to 1GB
        mm.seek(offset)
        data = mm.read(4096)  # Read 4KB
        checksums[offset] = hashlib.md5(data).hexdigest()
    
    print("Initial checksums captured. Waiting 2 seconds...")
    time.sleep(2)
    
    # Check for changes
    print("\nChecking for changes:")
    changed_count = 0
    for offset in range(0, 1024*1024*1024, 16*1024*1024):
        mm.seek(offset)
        data = mm.read(4096)
        new_checksum = hashlib.md5(data).hexdigest()
        
        if new_checksum != checksums[offset]:
            print(f"CHANGED at 0x{offset:08x}: {checksums[offset][:8]} -> {new_checksum[:8]}")
            changed_count += 1
            
            # Show what changed
            mm.seek(offset)
            old_data = mm.read(32)
            print(f"  First 32 bytes: {' '.join(f'{b:02x}' for b in old_data)}")
    
    if changed_count == 0:
        print("NO CHANGES DETECTED!")
        print("\nThis suggests:")
        print("1. The memory-backend-file might not be working correctly on macOS")
        print("2. QEMU might be using Copy-on-Write pages")
        print("3. Try using Linux instead of macOS for host")
    else:
        print(f"\n{changed_count} regions changed")
    
    mm.close()