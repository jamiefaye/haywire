#!/usr/bin/env python3

import socket
import time
import numpy as np
from PIL import Image
import sys

def read_memory_region(address, size):
    """Read a larger memory region from QEMU"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5.0)
        s.connect(('localhost', 4444))
        
        # Read initial prompt
        s.recv(1024)
        time.sleep(0.1)
        
        all_bytes = []
        bytes_read = 0
        
        while bytes_read < size:
            # Read in chunks of 256 bytes
            chunk_size = min(256, size - bytes_read)
            cmd = f"xp/{chunk_size}xb 0x{address + bytes_read:x}\n"
            s.send(cmd.encode())
            time.sleep(0.1)
            
            # Read response
            response = s.recv(8192).decode('utf-8', errors='ignore')
            
            # Parse hex values
            for line in response.split('\n'):
                if '0x' in line and ':' in line:
                    parts = line.split(':')
                    if len(parts) > 1:
                        hex_part = parts[1].strip()
                        for hex_str in hex_part.split():
                            if hex_str.startswith('0x'):
                                try:
                                    all_bytes.append(int(hex_str, 16))
                                    bytes_read += 1
                                    if bytes_read >= size:
                                        break
                                except:
                                    pass
            
            print(f"Read {bytes_read}/{size} bytes...", end='\r')
        
        s.close()
        return all_bytes[:size]
        
    except Exception as e:
        print(f"Error: {e}")
        return []

def display_memory_as_image(address, width=640, height=480):
    """Display memory as RGB image"""
    
    print(f"Reading memory from 0x{address:x}...")
    print(f"Image size: {width}x{height} RGB")
    
    # Read enough bytes for RGB image
    size = width * height * 3
    data = read_memory_region(address, size)
    
    if len(data) < size:
        print(f"\nWarning: Only read {len(data)} bytes, padding with zeros")
        data.extend([0] * (size - len(data)))
    
    # Convert to numpy array and reshape as RGB image
    arr = np.array(data, dtype=np.uint8)
    arr = arr.reshape((height, width, 3))
    
    # Create and save image
    img = Image.fromarray(arr, 'RGB')
    filename = f"memory_0x{address:x}_{width}x{height}.png"
    img.save(filename)
    print(f"\nSaved image to {filename}")
    
    # Also try as grayscale
    gray_arr = np.array(data[:width*height], dtype=np.uint8)
    gray_arr = gray_arr.reshape((height, width))
    gray_img = Image.fromarray(gray_arr, 'L')
    gray_filename = f"memory_0x{address:x}_{width}x{height}_gray.png"
    gray_img.save(gray_filename)
    print(f"Saved grayscale to {gray_filename}")
    
    # Show first 100 bytes as hex
    print("\nFirst 100 bytes:")
    for i in range(0, min(100, len(data)), 16):
        hex_str = ' '.join(f'{b:02x}' for b in data[i:i+16])
        print(f"  {i:04x}: {hex_str}")

if __name__ == "__main__":
    # Try the interesting address we found
    address = 0x40000000
    
    if len(sys.argv) > 1:
        address = int(sys.argv[1], 16)
    
    # Try different sizes
    display_memory_as_image(address, 320, 240)  # Small test
    print("\nYou can also try:")
    print(f"  python3 {sys.argv[0]} 0x80000000  # Different address")
    print(f"  python3 {sys.argv[0]} 0x40100000  # 1MB offset")