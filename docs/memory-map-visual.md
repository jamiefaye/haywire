# 6GB Memory File Layout (/tmp/haywire-vm-mem)

## File Structure (6GB total)
The file represents guest physical memory starting at 0x40000000 (1GB mark).
File offset 0x0 = Physical Address 0x40000000

```
FILE OFFSET         PHYSICAL ADDR       CONTENT
===========         =============       =======

0x00000000  ----    0x040000000  ┐     ACTUAL RAM DATA (linearly mapped)
(0 GB)                           │     --------------------------------
                                 │     • Contains kernel code/data
                                 │     • User process memory
                                 │     • Application data
                                 │     • This RAM is *accessible* via
0x40000000  ----    0x080000000  ├─────  PGD[0]'s linear mapping where
(1 GB)                           │       VA 0x0 → PA 0x40000000
                                 │       (Kernel can access using
                                 │        simple VA = PA - 0x40000000)
0x80000000  ----    0x0C0000000  ├─────• Continues...
(2 GB)                           │     • VA 0x80000000 → PA 0xC0000000
                                 │
0xC0000000  ----    0x100000000  ┘     • Total: 4GB of RAM with linear
(3 GB)                                   mapping for kernel access

                                       NORMAL GUEST RAM
                                       ----------------
0x100000000 ----    0x140000000  ┐     • User process memory
(4 GB)                           │     • Application data
                                 │     • Heap, stacks, etc.
                                 │     • Also contains some kernel
                                 │       dynamic allocations
                                 │
                                 ┘
                                       KERNEL STRUCTURES ZONE
0x136DEB000 ----                       ===============================
(~4.86 GB)                             **SWAPPER_PG_DIR** (kernel PGD)
                                       • The master kernel page table
                                       • 4KB page with 512 entries
                                       • Entry [0]: User linear map
                                       • Entry [256,507,511]: Kernel maps

0x138070000 ----                       Kernel PUD/PMD/PTE tables
0x138199000                            • PUD for PGD[256]
0x13819A000                            • PMD tables
0x13F646000                            • PUD for PGD[507]
0x13FFFF000                            • PUD for PGD[0] (user mapping)
(~5 GB)                                • Thousands of page tables

                                       More guest RAM...
0x140000000 ----    0x180000000  ┐
(5 GB)                           │     Continues...
                                 │
0x180000000 ----    0x1C0000000  ┘     End of 6GB file
(6 GB)              (7 GB PA)
```

## Memory Regions by Purpose

### 1. First 4GB of RAM (file offset 0x0 - 0x100000000)
   - **Physical addresses**: 0x40000000 - 0x140000000
   - **Contents**: Real RAM data - kernel, processes, applications
   - **Special property**: Has a linear mapping via PGD[0]
   - **Linear mapping**: Kernel can access this RAM using VA = PA - 0x40000000
   - **Size**: 4GB of actual RAM data

### 2. User Process Memory (scattered throughout)
   - Task_structs, mm_structs, process data
   - Dynamically allocated by SLUB/SLAB allocators

### 3. Kernel Page Tables (0x136DEB000 - 0x140000000)
   - **~115MB of page tables**
   - Includes swapper_pg_dir at 0x136DEB000
   - Thousands of PUD/PMD/PTE tables
   - All the page tables we've been walking are here

### 4. Kernel Dynamic Memory (scattered)
   - vmalloc allocations (mapped by PGD[256])
   - 9,503 scattered 4KB pages throughout RAM
   - Module memory

### 5. High Kernel Memory (scattered)
   - Mapped by PGD[507]
   - 440 pages scattered in RAM

## What's NOT in the File

### Below 0x40000000 (not in file, but referenced):
- **0x00000000 - 0x08000000**: Flash/ROM
- **0x08000000 - 0x40000000**: MMIO devices
- **0x3EFF0000**: PL011 UART (mapped by fixmap)

## Key Observations

1. **The file starts at PA 0x40000000**, not 0x0
2. **First 4GB is linearly mapped** for easy kernel access
3. **Kernel structures concentrated around 4.8-5GB mark**
4. **Page tables alone use ~115MB** of physical memory
5. **Most of the 6GB is regular RAM** available for processes

## Visual Summary
```
[0GB]  ████████ RAM with linear mapping (kernel code, user processes)
[1GB]  ████████ ↓ continues (actual RAM data)...
[2GB]  ████████ ↓ continues (actual RAM data)...
[3GB]  ████████ ↓ continues (actual RAM data)...
[4GB]  ░░░░░░░░ More RAM + scattered kernel allocations
[4.8GB]▓▓▓▓▓▓▓▓ KERNEL PAGE TABLES (swapper_pg_dir @ 0x136DEB000)
[5GB]  ░░░░░░░░ More RAM
[6GB]  (end of file)

Legend: █ = RAM with linear mapping  ░ = Regular RAM  ▓ = Kernel structures
```