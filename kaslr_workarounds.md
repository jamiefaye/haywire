# KASLR Workarounds for Process Tracking

## Methods to Defeat KASLR

### 1. From QEMU's Privileged Position
- **Read TTBR1_EL1**: Contains kernel page table base (randomized)
- **Read VBAR_EL1**: Exception vector base (randomized) 
- **Monitor SP_EL0**: Still contains valid task_struct pointers with KASLR

### 2. Calculate KASLR Slide
```python
# Method A: Using known kernel symbols
# The compiled kernel has fixed offsets between symbols
# If we find one symbol, we can calculate the slide

# init_task is always at a fixed offset from kernel base
expected_init_task_offset = 0x1709840  # Without KASLR
actual_init_task_va = read_from_sp_el0()  # From context switch
kaslr_slide = actual_init_task_va - expected_init_task_offset

# Method B: Using kernel page table base
ttbr1 = read_ttbr1_el1()  # From QEMU
# Kernel page tables are at fixed offset from kernel base
expected_pgdir_offset = 0x1dbf000
kaslr_slide = (ttbr1 & ~0xFFF) - expected_pgdir_offset
```

### 3. Pattern Matching Approach
```python
# Even with KASLR, we can identify structures by patterns
def find_task_structs_with_kaslr():
    # Task structs have recognizable patterns:
    # - Linked list pointers (next/prev) pointing to kernel VAs
    # - PID values in reasonable range (1-65535)
    # - comm field with ASCII process names
    # - State flags in known range
    
    # Scan memory for pattern:
    # [kernel_ptr][kernel_ptr][small_int][small_int]...
    # This likely indicates a task_struct
```

### 4. Use Physical Address Scanning
```python
# KASLR only randomizes VAs, not PAs!
# We can scan physical memory directly

def scan_physical_for_processes():
    # Physical memory layout is more predictable
    # Kernel is loaded at fixed PA (often 0x80000000)
    # Task structs allocated from slab caches in PA ranges
    
    for pa in range(0x80000000, 0x100000000, 0x1000):
        data = read_physical_memory(pa)
        if looks_like_task_struct(data):
            # Found one!
            extract_process_info(data)
```

### 5. Dynamic KASLR Detection
```python
# From QEMU hooks, detect KASLR at runtime
def detect_kaslr_offset():
    # Method 1: Compare sequential SP_EL0 values
    # Task structs are allocated from same slab
    # Their VAs will have consistent high bits despite KASLR
    
    # Method 2: Read kernel config from memory
    # CONFIG_RANDOMIZE_BASE location is findable
    
    # Method 3: Use GDB stub if available
    # Can query kernel symbols directly
```

## Implementation Strategy

### Phase 1: Detection
1. Check if KASLR is enabled (VAs outside expected range)
2. Calculate slide using multiple methods for verification
3. Store offset for session

### Phase 2: Adaptation  
1. Adjust all VA->PA translations by KASLR offset
2. Update pattern matching to use relative offsets
3. Continue using SP_EL0 tracking (still works!)

### Phase 3: Resilience
1. Scan physical memory directly (bypasses KASLR)
2. Use heuristics to identify kernel structures
3. Build process list from physical memory walk

## Key Advantages We Have

1. **QEMU sees everything** - KASLR can't hide from hypervisor
2. **Physical memory is not randomized** - Can scan PA space
3. **SP_EL0 tracking still works** - Context switches still update it
4. **Relative offsets are stable** - Within a structure, offsets don't change
5. **Pattern matching** - Task structs have recognizable signatures

## Example Code: KASLR-Resilient Scanner

```python
def find_processes_with_kaslr():
    # Step 1: Detect KASLR offset from SP_EL0
    sp_el0 = get_sp_el0_from_qemu()
    
    # VAs > 0xffff800080000000 indicate KASLR
    if sp_el0 > 0xffff800080000000:
        print("KASLR detected!")
        
        # Calculate offset
        # Normal kernel base: 0xffff800080000000
        # With KASLR: randomized within range
        base_va = sp_el0 & 0xffffffffff000000
        kaslr_offset = base_va - 0xffff800080000000
    else:
        kaslr_offset = 0
    
    # Step 2: Scan using physical addresses
    # This bypasses KASLR entirely!
    for pa in range(0xf0000000, 0x100000000, 0x8000):
        if is_task_struct_pattern(pa):
            decode_task_at_pa(pa)
    
    # Step 3: Use SP_EL0 tracking with offset
    task_va = sp_el0
    task_pa = va_to_pa_with_kaslr(task_va, kaslr_offset)
    decode_task_at_pa(task_pa)
```

## Conclusion

KASLR makes things harder but not impossible. Our approach using:
- QEMU's privileged position
- SP_EL0 tracking 
- Physical memory scanning
- Pattern matching

...would still work even with KASLR enabled. The key is that KASLR only randomizes virtual addresses, not:
- Physical memory layout
- Structure contents
- Relative offsets within structures
- Register usage patterns (SP_EL0)