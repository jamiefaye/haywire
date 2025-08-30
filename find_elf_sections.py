#!/usr/bin/env python3

import socket
import struct
import sys

def read_memory(addr, size):
    """Read memory via monitor protocol"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('localhost', 4444))
    s.recv(1024)  # Initial prompt
    
    data = bytearray()
    offset = 0
    
    while offset < size:
        chunk = min(256, size - offset)
        cmd = f"xp/{chunk}xb 0x{addr + offset:x}\n"
        s.send(cmd.encode())
        
        response = s.recv(8192).decode('utf-8', errors='ignore')
        
        # Parse hex bytes
        for line in response.split('\n'):
            if ':' in line:
                parts = line.split(':')
                if len(parts) > 1:
                    hex_part = parts[1].strip()
                    for hex_str in hex_part.split():
                        if hex_str.startswith('0x'):
                            try:
                                data.append(int(hex_str, 16))
                                offset += 1
                                if offset >= size:
                                    break
                            except:
                                pass
    
    s.close()
    return bytes(data)

def find_elf_headers(start_addr=0x40000000, end_addr=0x140000000, step=0x1000000):
    """Scan for ELF headers in memory"""
    print("Scanning for ELF headers...")
    elf_magic = b'\x7fELF'
    
    found_elfs = []
    
    for addr in range(start_addr, end_addr, step):
        # Read first 64 bytes
        try:
            data = read_memory(addr, 64)
            
            if data[:4] == elf_magic:
                print(f"\nFound ELF at 0x{addr:x}")
                
                # Parse ELF header
                if len(data) >= 52:
                    # e_ident (16 bytes)
                    ei_class = data[4]  # 1=32-bit, 2=64-bit
                    ei_data = data[5]   # 1=little, 2=big endian
                    
                    if ei_class == 2:  # 64-bit
                        # Parse 64-bit ELF header
                        e_type = struct.unpack('<H', data[16:18])[0]
                        e_machine = struct.unpack('<H', data[18:20])[0]
                        e_entry = struct.unpack('<Q', data[24:32])[0]
                        e_phoff = struct.unpack('<Q', data[32:40])[0]
                        e_shoff = struct.unpack('<Q', data[40:48])[0]
                        
                        elf_types = {1: 'REL', 2: 'EXEC', 3: 'DYN', 4: 'CORE'}
                        machines = {183: 'AARCH64', 62: 'X86_64'}
                        
                        print(f"  Type: {elf_types.get(e_type, e_type)}")
                        print(f"  Machine: {machines.get(e_machine, e_machine)}")
                        print(f"  Entry: 0x{e_entry:x}")
                        print(f"  Program headers: 0x{e_phoff:x}")
                        print(f"  Section headers: 0x{e_shoff:x}")
                        
                        # Try to read program headers
                        if e_phoff > 0 and e_phoff < 0x10000:
                            parse_program_headers(addr, e_phoff)
                        
                        found_elfs.append({
                            'addr': addr,
                            'type': e_type,
                            'entry': e_entry,
                            'sections': e_shoff
                        })
            
            elif b'VLC' in data or b'libvlc' in data:
                print(f"Found VLC-related data at 0x{addr:x}")
                
        except Exception as e:
            # Skip unreadable addresses
            pass
        
        # Progress indicator
        if addr % 0x10000000 == 0:
            print(f"  Scanning 0x{addr:x}...", end='\r')
    
    return found_elfs

def parse_program_headers(base_addr, ph_offset):
    """Parse ELF program headers to find segments"""
    try:
        # Read program header (56 bytes for 64-bit)
        ph_data = read_memory(base_addr + ph_offset, 56 * 8)  # Read up to 8 headers
        
        print("  Program segments:")
        for i in range(0, min(len(ph_data), 56*8), 56):
            if i + 56 > len(ph_data):
                break
                
            p_type = struct.unpack('<I', ph_data[i:i+4])[0]
            p_flags = struct.unpack('<I', ph_data[i+4:i+8])[0]
            p_offset = struct.unpack('<Q', ph_data[i+8:i+16])[0]
            p_vaddr = struct.unpack('<Q', ph_data[i+16:i+24])[0]
            p_filesz = struct.unpack('<Q', ph_data[i+32:i+40])[0]
            p_memsz = struct.unpack('<Q', ph_data[i+40:i+48])[0]
            
            seg_types = {1: 'LOAD', 2: 'DYNAMIC', 3: 'INTERP', 4: 'NOTE', 6: 'PHDR'}
            flags = []
            if p_flags & 1: flags.append('X')
            if p_flags & 2: flags.append('W')
            if p_flags & 4: flags.append('R')
            
            if p_type in seg_types:
                print(f"    {seg_types[p_type]:8} vaddr=0x{p_vaddr:016x} memsz=0x{p_memsz:08x} {''.join(flags)}")
                
                # LOAD segments with W flag are data segments - good for video buffers!
                if p_type == 1 and (p_flags & 2):
                    print(f"      ^-- Writable data segment, potential video buffer location!")
                    
    except Exception as e:
        print(f"  Could not parse program headers: {e}")

def find_vlc_memory():
    """Look specifically for VLC-related memory"""
    print("\nSearching for VLC-specific patterns...")
    
    # Common VLC strings to search for
    patterns = [
        b'libvlc',
        b'VLC media player',
        b'VideoLAN',
        b'avcodec',
        b'swscale',
        b'yuv420p',
        b'h264'
    ]
    
    # Scan in larger chunks where programs typically load
    for addr in range(0x400000000, 0x500000000, 0x10000000):
        try:
            data = read_memory(addr, 1024)
            for pattern in patterns:
                if pattern in data:
                    print(f"Found '{pattern.decode()}' at 0x{addr:x}")
                    
                    # Look around this area for heap/data segments
                    print(f"  Checking nearby memory for video buffers...")
                    check_for_video_buffer(addr)
                    break
        except:
            pass

def check_for_video_buffer(near_addr):
    """Check if memory looks like a video buffer"""
    # Video buffers are usually:
    # - Large (>100KB)
    # - Page-aligned
    # - Contain repeating patterns (frames)
    # - Have YUV or RGB data patterns
    
    for offset in [0x100000, 0x200000, 0x400000, 0x800000, 0x1000000]:
        addr = near_addr + offset
        try:
            sample = read_memory(addr, 1024)
            
            # Check for video-like patterns
            non_zero = sum(1 for b in sample if b != 0)
            if non_zero > 800:  # Mostly non-zero
                unique = len(set(sample))
                if 50 < unique < 200:  # Video-like entropy
                    print(f"    Potential video buffer at 0x{addr:x}")
                    print(f"      Entropy: {unique} unique values in 1KB")
                    
                    # Check for YUV patterns (Y often around 16-235)
                    y_like = sum(1 for b in sample if 16 <= b <= 235)
                    if y_like > 600:
                        print(f"      Likely YUV data ({y_like/10:.1f}% in Y range)")
                        
        except:
            pass

if __name__ == "__main__":
    print("ELF Section Scanner for Haywire")
    print("=" * 60)
    
    # Find ELF headers
    elfs = find_elf_headers()
    
    print(f"\n\nFound {len(elfs)} ELF headers")
    
    # Look for VLC specifically
    find_vlc_memory()
    
    print("\n\nSummary:")
    print("-" * 60)
    if elfs:
        print("ELF executables/libraries found:")
        for elf in elfs:
            print(f"  0x{elf['addr']:x} - Entry: 0x{elf['entry']:x}")
            
        print("\nHints:")
        print("- Writable LOAD segments are where heap/video buffers live")
        print("- Look for segments with size > 1MB for video buffers")
        print("- VLC likely allocates buffers near its code/data segments")