#!/usr/bin/env python3

import socket
import time
import numpy as np
from PIL import Image

def read_memory_sample(address, size=1024):
    """Read a small sample from an address"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(('localhost', 4444))
        s.recv(1024)  # Initial prompt
        time.sleep(0.05)
        
        # Read small sample
        cmd = f"xp/{size}xb 0x{address:x}\n"
        s.send(cmd.encode())
        time.sleep(0.1)
        
        response = s.recv(8192).decode('utf-8', errors='ignore')
        s.close()
        
        # Parse bytes
        bytes_data = []
        for line in response.split('\n'):
            if '0x' in line and ':' in line:
                parts = line.split(':')
                if len(parts) > 1:
                    hex_part = parts[1].strip()
                    for hex_str in hex_part.split():
                        if hex_str.startswith('0x'):
                            try:
                                bytes_data.append(int(hex_str, 16))
                            except:
                                pass
        return bytes_data
    except:
        return []

def scan_memory_ranges():
    """Scan memory to find potential video buffers"""
    
    print("Scanning memory for video patterns...")
    print("Looking for YUV/RGB patterns typical of video frames")
    print("-" * 60)
    
    # Memory ranges to scan (ARM64 Linux typical addresses)
    ranges = [
        (0x40000000, 0x80000000, 0x1000000),   # 1GB-2GB, step 16MB
        (0x80000000, 0x100000000, 0x2000000),  # 2GB-4GB, step 32MB  
        (0x100000000, 0x140000000, 0x4000000), # 4GB-5GB, step 64MB
    ]
    
    interesting_addresses = []
    
    for start, end, step in ranges:
        print(f"\nScanning 0x{start:x} to 0x{end:x}...")
        addr = start
        
        while addr < end:
            data = read_memory_sample(addr, 512)
            
            if len(data) > 256:
                # Analyze pattern
                non_zero = sum(1 for b in data if b != 0)
                
                # Check for video-like patterns:
                # 1. Not all zeros or all same value
                # 2. Some regularity (video has structure)
                
                if non_zero > 100:  # At least some data
                    # Check for potential YUV pattern (values clustered around 128)
                    yuv_like = sum(1 for b in data if 100 < b < 156) / len(data)
                    
                    # Check for RGB pattern (varied values)
                    unique = len(set(data))
                    
                    if yuv_like > 0.3 or unique > 100:
                        print(f"  0x{addr:x}: Interesting! ", end='')
                        print(f"YUV-like: {yuv_like:.1%}, Unique: {unique}")
                        
                        # Save a sample image
                        if len(interesting_addresses) < 5:  # Save first 5
                            save_sample(addr, data)
                            interesting_addresses.append(addr)
            
            addr += step
            
    print("\n" + "=" * 60)
    print(f"Found {len(interesting_addresses)} interesting regions:")
    for addr in interesting_addresses:
        print(f"  0x{addr:x}")
    
    return interesting_addresses

def save_sample(address, data):
    """Save a small sample as image"""
    # Try to display as small RGB image (16x16)
    size = min(len(data), 16*16*3)
    data = data[:size]
    
    if len(data) < 16*16*3:
        data.extend([0] * (16*16*3 - len(data)))
    
    arr = np.array(data[:16*16*3], dtype=np.uint8).reshape((16, 16, 3))
    img = Image.fromarray(arr, 'RGB')
    img = img.resize((128, 128), Image.NEAREST)  # Scale up for visibility
    filename = f"sample_0x{address:x}.png"
    img.save(filename)
    print(f"    Saved sample to {filename}")

if __name__ == "__main__":
    interesting = scan_memory_ranges()
    
    if interesting:
        print("\nTo examine these regions more closely:")
        for addr in interesting[:3]:
            print(f"  python3 display_memory.py 0x{addr:x}")