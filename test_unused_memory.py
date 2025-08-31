#!/usr/bin/env python3
"""
Test what unused guest memory looks like
"""

import mmap
import struct
import sys

def check_unused_memory(filepath="/tmp/haywire-vm-mem", sample_size=0x100000):
    """Sample unused memory regions"""
    
    try:
        with open(filepath, "r+b") as f:
            # Map the file
            mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            
            # Check end of RAM (usually unused)
            end_offset = len(mm) - sample_size
            mm.seek(end_offset)
            data = mm.read(sample_size)
            
            # Count patterns
            zeros = data.count(b'\x00')
            ffs = data.count(b'\xff')
            
            print(f"Last {sample_size//1024}KB of guest RAM:")
            print(f"  Zeros: {zeros} bytes ({100*zeros/sample_size:.1f}%)")
            print(f"  0xFF:  {ffs} bytes ({100*ffs/sample_size:.1f}%)")
            print(f"  Other: {sample_size-zeros-ffs} bytes")
            
            # Show first 256 bytes as hex
            print("\nFirst 256 bytes of unused region:")
            for i in range(0, min(256, len(data)), 16):
                hex_str = ' '.join(f'{b:02x}' for b in data[i:i+16])
                ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+16])
                print(f"  {end_offset+i:08x}: {hex_str:<48} {ascii_str}")
            
            mm.close()
            
    except FileNotFoundError:
        print(f"Memory backend file not found: {filepath}")
        print("Make sure QEMU is running with memory-backend-file")
        sys.exit(1)

if __name__ == "__main__":
    check_unused_memory()