#!/usr/bin/env python3

import socket
import time
import sys

def read_memory(address, size=256):
    """Read memory from QEMU monitor"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(('localhost', 4444))
        
        # Read initial prompt
        s.recv(1024)
        time.sleep(0.1)
        
        # Send memory read command
        cmd = f"xp/{size}xb 0x{address:x}\n"
        s.send(cmd.encode())
        time.sleep(0.2)
        
        # Read response
        response = s.recv(8192).decode('utf-8', errors='ignore')
        
        # Parse hex values from response
        values = []
        for line in response.split('\n'):
            if '0x' in line and ':' in line:
                # Extract hex values after the colon
                parts = line.split(':')
                if len(parts) > 1:
                    hex_part = parts[1].strip()
                    # Parse individual bytes
                    for hex_str in hex_part.split():
                        if hex_str.startswith('0x'):
                            try:
                                values.append(int(hex_str, 16))
                            except:
                                pass
        
        s.close()
        return values
    except Exception as e:
        print(f"Error: {e}")
        return []

def find_video_patterns():
    """Search for video buffer patterns in memory"""
    
    # Common memory regions to check (ARM64 Linux)
    test_addresses = [
        0x100000000,      # 4GB mark
        0x200000000,      # 8GB mark  
        0x40000000,       # 1GB mark
        0x80000000,       # 2GB mark
        0xffff000000000000,  # Kernel space start (ARM64)
    ]
    
    print("Searching for video buffer patterns...")
    print("Looking for regions with high entropy (video data)")
    print("-" * 60)
    
    for addr in test_addresses:
        print(f"\nChecking 0x{addr:016x}...")
        data = read_memory(addr, 256)
        
        if data:
            # Check for patterns
            non_zero = sum(1 for b in data if b != 0)
            unique = len(set(data))
            
            print(f"  Read {len(data)} bytes")
            print(f"  Non-zero bytes: {non_zero}/{len(data)}")
            print(f"  Unique values: {unique}")
            
            # High entropy suggests compressed video or image data
            if unique > 100 and non_zero > 128:
                print(f"  *** INTERESTING! High entropy region - might be video data ***")
                print(f"  First 32 bytes: {' '.join(f'{b:02x}' for b in data[:32])}")
            elif non_zero > 0:
                print(f"  First 32 bytes: {' '.join(f'{b:02x}' for b in data[:32])}")
        else:
            print(f"  Could not read memory at this address")
        
        time.sleep(0.5)  # Don't overwhelm the monitor

if __name__ == "__main__":
    find_video_patterns()