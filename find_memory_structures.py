#!/usr/bin/env python3
"""
Find recognizable memory structures in guest RAM
"""

import mmap
import struct
import sys

def find_structures(filepath="/tmp/haywire-vm-mem"):
    """Look for common memory patterns"""
    
    try:
        with open(filepath, "r+b") as f:
            mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
            file_size = len(mm)
            
            print(f"Scanning {file_size//1024//1024}MB for memory structures...\n")
            
            # Pattern 1: Stack-like structures (return addresses)
            print("=== Looking for stacks (ARM64 return addresses) ===")
            # ARM64 kernel addresses often start with 0xFFFF
            for offset in range(0, min(file_size, 256*1024*1024), 1024*1024):  # Every 1MB
                mm.seek(offset)
                chunk = mm.read(4096)
                
                # Count potential return addresses
                ptr_count = 0
                for i in range(0, len(chunk)-8, 8):
                    val = struct.unpack('<Q', chunk[i:i+8])[0]
                    # ARM64 kernel space: 0xFFFF000000000000+
                    # ARM64 user space: < 0x0000800000000000
                    if (0xFFFF000000000000 <= val <= 0xFFFFFFFFFFFFFFFF) or \
                       (0x0000000000400000 <= val <= 0x0000800000000000):
                        ptr_count += 1
                
                if ptr_count > 20:  # Many pointers suggest stack/heap
                    print(f"  0x{offset:08x}: {ptr_count} potential pointers")
                    # Show sample
                    for i in range(0, min(64, len(chunk)), 8):
                        val = struct.unpack('<Q', chunk[i:i+8])[0]
                        if val > 0x1000:
                            print(f"    +{i:02x}: 0x{val:016x}")
                    print()
            
            # Pattern 2: String tables (kernel messages, etc)
            print("=== Looking for string regions ===")
            for offset in range(0, min(file_size, 256*1024*1024), 4*1024*1024):  # Every 4MB
                mm.seek(offset)
                chunk = mm.read(4096)
                
                # Count printable ASCII
                printable = sum(1 for b in chunk if 32 <= b <= 126)
                if printable > len(chunk) * 0.7:  # 70%+ printable
                    print(f"  0x{offset:08x}: {printable}/{len(chunk)} printable chars")
                    # Show preview
                    text = ''.join(chr(b) if 32 <= b <= 126 else '.' for b in chunk[:128])
                    print(f"    Preview: {text[:80]}")
                    print()
            
            # Pattern 3: Page tables (aligned, specific patterns)
            print("=== Looking for page tables ===")
            for offset in range(0, min(file_size, 256*1024*1024), 2*1024*1024):  # Every 2MB
                if offset % (2*1024*1024) != 0:  # Page tables are aligned
                    continue
                    
                mm.seek(offset)
                chunk = mm.read(4096)
                
                # Check for page table entries (bit patterns)
                pte_count = 0
                for i in range(0, len(chunk)-8, 8):
                    val = struct.unpack('<Q', chunk[i:i+8])[0]
                    # Check for valid PTE bits (present, etc)
                    if (val & 0x1) and (val & 0xFFFFF00000000000) == 0:
                        pte_count += 1
                
                if pte_count > 100:
                    print(f"  0x{offset:08x}: Possible page table ({pte_count} PTEs)")
            
            # Pattern 4: ELF headers (executables in memory)
            print("\n=== Looking for ELF headers ===")
            elf_magic = b'\x7fELF'
            pos = 0
            while pos < min(file_size, 512*1024*1024):
                pos = mm.find(elf_magic, pos)
                if pos == -1:
                    break
                print(f"  0x{pos:08x}: ELF header found")
                mm.seek(pos)
                header = mm.read(64)
                # Parse basic ELF info
                if len(header) >= 20:
                    ei_class = header[4]
                    ei_data = header[5]
                    e_machine = struct.unpack('<H', header[18:20])[0]
                    print(f"    Class: {'64-bit' if ei_class == 2 else '32-bit'}")
                    print(f"    Machine: 0x{e_machine:x} ({'ARM64' if e_machine == 0xB7 else 'Unknown'})")
                pos += 1
            
            mm.close()
            
    except FileNotFoundError:
        print(f"Memory backend file not found: {filepath}")
        sys.exit(1)

if __name__ == "__main__":
    find_structures()