#!/usr/bin/env python3
"""
Find active framebuffer by looking for rapidly changing memory regions
"""

import mmap
import time
import hashlib
import sys

def find_changing_regions(filepath="/tmp/haywire-vm-mem", 
                         region_size=1920*1080*4,  # One frame of 1080p RGBA
                         sample_count=10):
    """Find memory regions that change frequently"""
    
    try:
        with open(filepath, "r+b") as f:
            mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            file_size = len(mm)
            
            print(f"Scanning {file_size//1024//1024}MB for changing regions...")
            print(f"Looking for regions of ~{region_size//1024//1024}MB")
            
            # Track changes for different offsets
            change_counts = {}
            previous_hashes = {}
            
            # Sample at various offsets
            stride = 16 * 1024 * 1024  # Check every 16MB
            
            for sample in range(sample_count):
                print(f"\nSample {sample+1}/{sample_count}")
                
                for offset in range(0, file_size - region_size, stride):
                    # Read a small sample from this region
                    mm.seek(offset)
                    sample_data = mm.read(4096)  # Just first 4KB
                    
                    # Hash it
                    current_hash = hashlib.md5(sample_data).digest()
                    
                    # Check if it changed
                    if offset in previous_hashes:
                        if current_hash != previous_hashes[offset]:
                            change_counts[offset] = change_counts.get(offset, 0) + 1
                            
                            # Check for patterns suggesting video
                            if sample_data[0:4] != b'\x00\x00\x00\x00':  # Non-zero
                                if change_counts[offset] >= 3:  # Changed 3+ times
                                    print(f"  Active region at 0x{offset:08x} "
                                          f"(changed {change_counts[offset]} times)")
                    
                    previous_hashes[offset] = current_hash
                
                time.sleep(0.1)  # Wait 100ms between samples
            
            # Report most active regions
            print("\n=== Most Active Regions ===")
            active = sorted([(count, offset) for offset, count in change_counts.items()], 
                          reverse=True)[:10]
            
            for count, offset in active:
                if count >= 2:
                    print(f"0x{offset:08x}: Changed {count}/{sample_count} times")
                    
                    # Show first few bytes
                    mm.seek(offset)
                    preview = mm.read(32)
                    hex_str = ' '.join(f'{b:02x}' for b in preview[:16])
                    print(f"  Preview: {hex_str}")
            
            mm.close()
            
    except FileNotFoundError:
        print(f"Memory backend file not found: {filepath}")
        sys.exit(1)

if __name__ == "__main__":
    find_changing_regions()