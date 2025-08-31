#!/usr/bin/env python3
import socket
import json
import time
import sys

def send_qmp_command(sock, command):
    """Send QMP command and get response"""
    sock.send((json.dumps(command) + '\n').encode())
    response = b""
    while b'\n' not in response:
        response += sock.recv(4096)
    return json.loads(response.decode().split('\n')[0])

def read_memory_gdb(gdb_sock, address, size):
    """Read memory using GDB protocol"""
    # Format: m addr,length
    cmd = f"m{address:x},{size:x}"
    packet = f"${cmd}#{calculate_checksum(cmd):02x}"
    gdb_sock.send(packet.encode())
    
    response = b""
    while True:
        data = gdb_sock.recv(4096)
        response += data
        if b'#' in data:
            break
    
    # Parse response
    if response.startswith(b'+$'):
        hex_data = response[2:].split(b'#')[0]
        if hex_data and hex_data != b'E':
            return bytes.fromhex(hex_data.decode())
    return None

def calculate_checksum(data):
    """Calculate GDB packet checksum"""
    return sum(ord(c) for c in data) & 0xff

def scan_memory_regions():
    """Scan various memory regions to find active areas"""
    
    # Connect to QMP
    qmp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    qmp.connect(('localhost', 4445))
    
    # QMP handshake
    greeting = qmp.recv(4096)
    qmp.send(b'{"execute": "qmp_capabilities"}\n')
    qmp.recv(4096)
    
    # Connect to GDB
    gdb = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    gdb.connect(('localhost', 1234))
    gdb.recv(1024)  # Greeting
    
    # Common memory regions to check (ARM64 typical layouts)
    regions = [
        (0x00000000, "Low memory / vectors"),
        (0x40000000, "Typical RAM start (1GB)"),
        (0x80000000, "Alternative RAM (2GB)"),
        (0xC0000000, "High RAM (3GB)"),
        (0x100000000, "Extended RAM (4GB+)"),
        (0x140000000, "Extended RAM (5GB)"),
        (0x180000000, "Extended RAM (6GB)"),
        (0x08000000, "Device memory"),
        (0x09000000, "PCI/Device region"),
        (0x0a000000, "PCI Config"),
        (0x10000000, "Platform devices"),
        (0x3eff0000, "VGA/Graphics region"),
        (0x50000000, "GIC region"),
    ]
    
    print("Scanning memory regions for activity...")
    print("-" * 60)
    
    active_regions = []
    
    for base_addr, description in regions:
        try:
            # Sample 4KB at each region
            sample_size = 4096
            
            # Take two samples with a small delay
            sample1 = read_memory_gdb(gdb, base_addr, sample_size)
            if not sample1:
                continue
                
            time.sleep(0.5)
            
            sample2 = read_memory_gdb(gdb, base_addr, sample_size)
            if not sample2:
                continue
            
            # Count differences
            changes = sum(1 for a, b in zip(sample1, sample2) if a != b)
            
            # Check for non-zero content
            non_zero = sum(1 for b in sample1 if b != 0)
            
            if non_zero > 0:  # Has some content
                status = ""
                if changes > 0:
                    status = f"ACTIVE! {changes} bytes changed"
                    active_regions.append((base_addr, description, changes))
                elif non_zero > 100:  # Significant content
                    status = f"Static content ({non_zero} non-zero bytes)"
                else:
                    status = f"Sparse ({non_zero} non-zero bytes)"
                    
                print(f"0x{base_addr:08x}: {description:20s} - {status}")
                
                # If very active, scan more precisely
                if changes > 10:
                    print(f"  Scanning more thoroughly...")
                    for offset in range(0, 0x100000, 0x10000):  # Scan 1MB in 64KB chunks
                        addr = base_addr + offset
                        s1 = read_memory_gdb(gdb, addr, 256)
                        time.sleep(0.1)
                        s2 = read_memory_gdb(gdb, addr, 256)
                        if s1 and s2:
                            diff = sum(1 for a, b in zip(s1, s2) if a != b)
                            if diff > 0:
                                print(f"    0x{addr:08x}: {diff} changes in 256 bytes")
                
        except Exception as e:
            pass  # Region not accessible
    
    print("-" * 60)
    print("\nMost active regions:")
    for addr, desc, changes in sorted(active_regions, key=lambda x: x[2], reverse=True)[:5]:
        print(f"  0x{addr:08x}: {desc} ({changes} changes)")
    
    # Also check what guest sees
    print("\nChecking guest memory map via QMP...")
    cmd = {"execute": "human-monitor-command", 
           "arguments": {"command-line": "info mtree"}}
    response = send_qmp_command(qmp, cmd)
    if 'return' in response:
        lines = response['return'].split('\n')
        print("Guest RAM regions:")
        for line in lines[:50]:  # First 50 lines
            if 'ram' in line.lower() or 'system' in line.lower():
                print(f"  {line.strip()}")
    
    qmp.close()
    gdb.close()

if __name__ == "__main__":
    scan_memory_regions()