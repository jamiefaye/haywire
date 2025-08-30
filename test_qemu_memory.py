#!/usr/bin/env python3

import socket
import time
import sys

def test_monitor_connection():
    """Test reading memory from QEMU monitor"""
    try:
        # Connect to monitor
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(1.0)  # 1 second timeout
        s.connect(('localhost', 4444))
        
        # Read initial prompt
        data = s.recv(1024)
        print(f"Initial: {data[:50]}...")
        
        # Send a simple command
        s.send(b"info status\n")
        time.sleep(0.1)
        
        # Read response
        response = s.recv(1024)
        print(f"Status: {response.decode('utf-8', errors='ignore')[:100]}...")
        
        # Try reading memory
        print("\nTrying to read memory at 0x100000000...")
        s.send(b"xp/16xb 0x100000000\n")
        time.sleep(0.1)
        
        response = s.recv(4096)
        print(f"Memory: {response.decode('utf-8', errors='ignore')}")
        
        s.close()
        print("\nSuccess! Monitor is responding.")
        
    except socket.timeout:
        print("ERROR: Connection timed out!")
    except Exception as e:
        print(f"ERROR: {e}")

if __name__ == "__main__":
    test_monitor_connection()