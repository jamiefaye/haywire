# Discovering Unmapped Pages in Linux Kernel Memory

## Overview
After PTE scanning, we typically find only 30-50% of physical pages mapped. The remaining pages aren't "lost" - they're tracked by various kernel subsystems.

## Categories of Unmapped Pages

### 1. Page Cache (File Cache)
**What**: Cached file contents from disk
**Percentage**: Often 20-40% of total RAM
**Kernel Structures**:
```c
struct address_space {
    struct xarray i_pages;     // Radix tree/xarray of cached pages
    unsigned long nrpages;     // Number of pages
    ...
}

struct inode {
    struct address_space *i_mapping;  // Points to cached pages
    ...
}
```
**Discovery Method**:
- Find superblock list
- Walk all inodes
- For each inode, check i_mapping->i_pages
- Extract page frame numbers from xarray

### 2. Free Pages (Buddy Allocator)
**What**: Unallocated pages ready for use
**Percentage**: 5-30% depending on system load
**Kernel Structures**:
```c
struct zone {
    struct free_area free_area[MAX_ORDER];  // Free lists by order
    ...
}

struct free_area {
    struct list_head free_list[MIGRATE_TYPES];
    unsigned long nr_free;
}
```
**Discovery Method**:
- Find `struct zone` (usually 2-3 zones: DMA, DMA32, Normal)
- Walk free_area[0] through free_area[10]
- Each order has 2^order contiguous pages

### 3. SLAB/SLUB Caches
**What**: Kernel object cache pools
**Percentage**: 5-15% of RAM
**Kernel Structures**:
```c
struct kmem_cache {
    struct list_head list;     // Link in slab_caches list
    const char *name;         // Cache name (e.g., "task_struct")
    unsigned int size;        // Object size
    struct kmem_cache_node *node[MAX_NUMNODES];
    ...
}

struct page {
    union {
        struct {  // SLUB
            struct kmem_cache *slab_cache;
            void *freelist;
            ...
        };
    };
}
```
**Discovery Method**:
- Find global `slab_caches` list
- Walk each kmem_cache
- For each cache, find partial and full slabs
- Mark those pages as "slab: cache_name"

### 4. Per-CPU Pages
**What**: CPU-local page lists for fast allocation
**Percentage**: Usually < 1%
**Kernel Structures**:
```c
struct per_cpu_pages {
    int count;
    int high;
    int batch;
    struct list_head lists[MIGRATE_PCPTYPES];
}
```
**Discovery Method**:
- Find per-CPU data areas
- Extract per_cpu_pageset for each CPU
- Walk the page lists

### 5. Anonymous Pages (Swap Cache)
**What**: Pages that might be swapped out
**Percentage**: Variable
**Kernel Structures**:
```c
struct swap_info_struct {
    unsigned long *swap_map;    // Usage count for each swap slot
    struct address_space *swap_address_space;
    ...
}
```

### 6. Memory Zones Metadata
**What**: Pages used for struct page array itself
**Percentage**: ~1.5% of RAM (64 bytes per 4KB page)
**Discovery Method**:
- Find mem_map or vmemmap base
- These pages hold the `struct page` array

## Implementation Approach

### Step 1: Find Memory Zones
```c
// Key symbols to find:
// - node_data[] - Array of NUMA nodes
// - contig_page_data - Non-NUMA systems
```

### Step 2: Walk Zone Free Lists
```python
def discover_free_pages(zone_addr):
    free_pages = []
    for order in range(11):  # 0 to 10
        free_area = read_struct(zone_addr + offset_of_free_area[order])
        # Walk the free list
        for page in walk_list(free_area.free_list):
            # This page and 2^order-1 following pages are free
            free_pages.append((page_to_pfn(page), 2**order))
    return free_pages
```

### Step 3: Find Page Cache
```python
def discover_page_cache():
    # Find all superblocks
    for sb in walk_superblock_list():
        # Walk inode list
        for inode in walk_inode_list(sb):
            mapping = read_pointer(inode + offset_i_mapping)
            if mapping:
                # Walk the xarray/radix tree
                for page in walk_xarray(mapping + offset_i_pages):
                    mark_page_as_cache(page, inode)
```

### Step 4: Identify SLAB Pages
```python
def discover_slab_pages():
    # Find slab_caches list head
    for cache in walk_list(slab_caches):
        cache_name = read_string(cache + offset_name)
        # Walk each CPU's partial list
        for cpu in range(nr_cpus):
            for page in walk_partial_list(cache, cpu):
                mark_page_as_slab(page, cache_name)
```

## Expected Coverage After Full Discovery

With complete discovery, we should be able to classify:
- **30-50%**: User process mappings (current PTE scan)
- **20-40%**: Page cache (file cache)
- **5-30%**: Free pages
- **5-15%**: SLAB/SLUB caches
- **1-2%**: Kernel code and data
- **1-2%**: Struct page array
- **1-5%**: Other (network buffers, DMA, etc.)

**Total**: Should achieve 90-95% page classification

## Challenges

1. **Kernel Version Differences**: Structures change between versions
2. **CONFIG Options**: Different configs change struct layouts
3. **KASLR**: Makes finding initial structures harder
4. **Live System**: Data structures constantly changing
5. **Huge Pages**: Need special handling for 2MB/1GB pages

## Priority Order for Implementation

1. **Free pages** - Easiest, zone structure is stable
2. **Page cache** - High value, shows file I/O patterns
3. **SLAB caches** - Shows kernel memory usage patterns
4. **Per-CPU pages** - Small but completes the picture

## Visual Representation Ideas

Could show unmapped pages with different colors:
- Gray: Unknown/unmapped
- Green: Free (available)
- Yellow: Page cache (file cache)
- Orange: SLAB cache
- Red: Anonymous/swap
- Blue: Kernel internal

This would give a complete view of physical memory usage, not just process mappings.