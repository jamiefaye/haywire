# vm_area_struct Layout in Modern Linux

## Standard Layout (Linux 6.x)
```c
struct vm_area_struct {
    /* The first cache line is mostly read-only */
    union {
        struct {
            /* VMA covers [vm_start, vm_end) addresses */
            unsigned long vm_start;     // offset 0x00
            unsigned long vm_end;       // offset 0x08
        };
        struct rcu_head vm_rcu;     // Only used after freeing
    };

    struct mm_struct *vm_mm;       // offset 0x10
    pgprot_t vm_page_prot;         // offset 0x18
    unsigned long vm_flags;        // offset 0x20

    /* ... more fields ... */
};
```

## With Maple Tree (Linux 6.1+)
In newer kernels, VMAs might have additional fields or different organization:

```c
struct vm_area_struct {
    /* First two fields should still be vm_start and vm_end */
    unsigned long vm_start;         // offset 0x00
    unsigned long vm_end;           // offset 0x08

    /* Maple tree might add per-VMA tree node info */
    struct mm_struct *vm_mm;        // offset 0x10
    pgprot_t vm_page_prot;
    unsigned long vm_flags;         // offset 0x20 or 0x28

    /* ... */
};
```

## Why We See vm_start=0x1, vm_end=0x0

When we read a supposed vm_area_struct at 0xffff0000c1640000 and get:
- vm_start = 0x1
- vm_end = 0x0

This is clearly wrong because:
1. vm_end can't be less than vm_start
2. vm_start = 0x1 is not a valid user address (too low)
3. These values suggest we're reading the wrong memory

## Possible Explanations:

### 1. The pointer is to something else
The value 0xffff0000c1640000 might not be a vm_area_struct pointer at all.
It could be:
- A different kernel structure
- An encoded value with flags
- A freed/reused memory location

### 2. The values like 0xea6f57989000 are the keys
In a maple tree storing VMAs, the keys are virtual addresses.
The strange 0xe... values might be:
- Virtual address ranges (the keys)
- With encoding in high bits
- The vm_area_struct pointers are stored separately

### 3. Maple tree leaf format is different than expected
The leaf might store:
- Pivots (boundary addresses)
- Keys (virtual addresses)
- Values (vm_area_struct pointers)
All interleaved in a specific pattern we haven't decoded yet.