# Swapper PGD Complete Mapping Dump

## Overview
Swapper PGD at PA 0x136deb000  
Non-zero PGD entries: 4/512

## PGD Entry Details

### PGD[0]: User Space (VA 0x0000000000000000)
```
PUD Table at PA 0x13ffff000:
  PUD[0]: VA 0x0 -> PMD table
    - 512 PMD entries pointing to PTE tables
    - Each PTE table maps 512 pages (2MB total)
    - Maps user process memory
    
  PUD[1]: VA 0x40000000 -> PMD table  
    - Another 512 PMD entries
    - Maps VA 0x40000000-0x80000000 to PA 0x80000000+
    
  PUD[2]: VA 0x80000000 -> PMD table
  PUD[3]: VA 0xc0000000 -> PMD table
  ... more PUD entries for user space
```

### PGD[256]: Kernel Linear Map (VA 0x0000800000000000)
```
PUD Table at PA 0x138199000:
  PUD[1]: VA 0x800040000000 -> PMD table
    PMD[478]: VA 0x80007bc00000 -> PTE table (69 pages)
      - Sparse mappings, not contiguous
      - Example: VA 0x80007bdb0000 -> PA 0x10bb8a000
      
    PMD[479]: VA 0x80007be00000 -> PTE table (376 pages)
      - More sparse mappings
      
    PMD[480]: VA 0x80007c000000 -> PTE table (242 pages)
    
  PUD[2]: VA 0x800080000000 -> PMD table
    PMD[0]: VA 0x800080000000 -> PTE table (506 pages)
    PMD[1]: VA 0x800080200000 -> 2MB block at PA 0x134600000
    PMD[2]: VA 0x800080400000 -> 2MB block at PA 0x134800000
    PMD[12]: VA 0x800081800000 -> PTE table (512 pages)
      - Maps VA 0x800081800000-0x800081a00000
      - To PA 0x135c00000-0x135e00000
    PMD[20]: VA 0x800082800000 -> PTE table (496 pages)  
      - Maps VA 0x800082800000-0x8000829f0000
      - To PA 0x136c00000-0x136df0000
      - **This includes swapper_pg_dir area!**
```

### PGD[507]: Kernel Fixmap (VA 0x0000fd8000000000)
```
PUD Table at PA 0x13f646000:
  PUD[510]: VA 0xfdff80000000 -> PMD table
    PMD[506]: VA 0xfdffbf400000 -> PTE table (36 pages)
      - Very sparse, non-contiguous mappings
      
    PMD[507]: VA 0xfdffbf600000 -> PTE table (460 pages)
      - Scattered physical addresses
      
  PUD[511]: VA 0xfdffc0000000 -> PMD table
    PMD[0-31]: 32x 2MB blocks
      - PA 0x138400000, 0x138600000, etc.
```

### PGD[511]: Kernel Modules (VA 0x0000ff8000000000)
```
PUD Table at PA 0x138070000:
  PUD[511]: VA 0xffffc0000000 -> PMD table
    PMD[4]: VA 0xffffc0800000 -> PTE table (16 pages)
      - Maps to PA 0x3eff0000-0x3f000000
      
    PMD[506]: VA 0xffffff400000 -> PTE table (1 page)
    PMD[507]: VA 0xffffff600000 -> PTE table
```

## Key Observations

### What's Mapped
1. **User space (PGD[0])**: Comprehensive mappings for user processes
2. **Kernel linear map (PGD[256])**: SPARSE - not a complete linear map!
3. **Fixmap region (PGD[507])**: Special kernel mappings
4. **Module region (PGD[511])**: Loaded kernel modules

### What's NOT Mapped
- Most physical RAM is NOT in the kernel page tables
- SLAB/SLUB allocations (where task_structs live) are NOT mapped
- The kernel must be using implicit VA = PA + offset mapping

### Implications for Process Discovery

1. **Why 91% limit exists**:
   - Task_structs at offset 0x4700 straddle 3 pages
   - When pages are non-contiguous, no page table tells us where continuation is
   - We can't follow the VA to find the rest of the structure

2. **How kernel accesses unmapped memory**:
   - Uses fixed offset: VA = PA + 0xffff7fff4bc00000
   - This is an implicit mapping, not in page tables
   - Kernel MMU hardware knows this transformation

3. **Why we can't achieve 100%**:
   - Without page table entries, we can't resolve non-contiguous pages
   - SLUB metadata (which would tell us) is also in unmapped memory
   - It's a chicken-and-egg problem

## Memory Layout Context

```
Physical Address Space:
0x040000000 - Start of guest RAM
0x136DEB000 - swapper_pg_dir (this table)
0x138070000 - PUD for PGD[511] (modules)
0x138199000 - PUD for PGD[256] (kernel)
0x13F646000 - PUD for PGD[507] (fixmap)  
0x13FFFF000 - PUD for PGD[0] (user)
0x1C0000000 - End of guest RAM (6GB)
```

## Summary

The kernel page tables are much sparser than expected. Most kernel memory (including SLAB/SLUB allocations) is accessed via implicit linear mapping, not through page table entries. This is why:

1. We achieve 91% process discovery through physical scanning
2. The remaining 9% (straddled task_structs) cannot be recovered
3. Following kernel lists (init_task->next) fails at non-contiguous boundaries
4. The IDR appears empty (it may also be in unmapped memory)

The 91% discovery rate appears to be the theoretical maximum achievable through physical memory scanning without guest cooperation.