# What /proc/maps Skips and Filters

## 1. The VMA Iterator Itself Filters

The iterator uses `vma_next()` which calls `mas_find()`:

```c
static inline struct vm_area_struct *vma_next(struct vma_iterator *vmi)
{
    return mas_find(&vmi->mas, ULONG_MAX);
}
```

`mas_find()` in the maple tree:
- **Skips over NULL entries**
- **Skips over internal nodes** (only returns leaf values)
- **Skips over metadata/pivots** in the nodes
- **Only returns actual values** stored in the tree

## 2. The /proc/maps Code Filters VMAs

In `show_map()` and related functions:

```c
static int show_map(struct seq_file *m, void *v)
{
    struct vm_area_struct *vma = v;

    // Some VMAs might be skipped based on:
    // 1. Permissions (if the process doesn't have rights)
    // 2. Special VMAs (gate vmas, etc.)

    show_map_vma(m, vma);
    return 0;
}
```

## 3. Special VMA Filtering

Gate VMAs and special mappings might be handled differently:

```c
static void show_map_vma(struct seq_file *m, struct vm_area_struct *vma)
{
    // Gate VMAs might be shown differently
    if (is_gate_vma(vma)) {
        // Special handling
    }

    // Skip VMAs with no actual mapping
    if (!vma->vm_file && !vma->vm_ops && ...) {
        // Might format differently or skip
    }
}
```

## 4. What mas_find() Actually Does

The maple tree iterator (`mas_find()`) is smart:

```c
void *mas_find(struct ma_state *mas, unsigned long max)
{
    if (unlikely(mas_is_none(mas) || mas_is_paused(mas)))
        mas->node = MAS_START;

    if (unlikely(mas_is_start(mas))) {
        /* First run, walk to the first value */
        ...
    }

    if (unlikely(!mas_searchable(mas)))
        return NULL;

    /* This is the key part - it walks through the tree
     * and ONLY returns actual stored values, not:
     * - Keys/indices
     * - Metadata
     * - Pivots
     * - Internal node pointers
     */
    return mas_next_slot(mas, max, false);
}
```

## KEY INSIGHT: Maple Tree Leaf Structure

In a maple tree storing VMAs, the leaf nodes contain:

1. **Pivots** - The boundary addresses (keys) that define ranges
2. **Slots** - The actual values (vm_area_struct pointers)
3. **Metadata** - Information about the node structure

The iterator (`mas_find()`) knows how to:
- Read the metadata to understand the node format
- Skip over the pivots/keys
- Return only the actual values (VMA pointers)

## What This Means for Our Code

We're seeing values like:
- `0xea6f57989000` - These are likely **pivots/keys** (address boundaries)
- `0xffff0000c1640000` - These should be **VMA pointers** but aren't valid
- `0x100071` - Small values that are **metadata or indices**

The problem is we're not correctly identifying which slots contain:
1. Pivots (keys/boundaries)
2. Values (VMA pointers)
3. Metadata

## Maple Leaf Node Layout (Simplified)

```
Leaf Node:
[Header/Type info]
[Pivot array] - Contains boundary addresses (the keys)
[Slot array]  - Contains the actual values (VMA pointers)
[Metadata]    - Contains slot count, gaps, etc.
```

We need to:
1. Read the metadata to understand the node format
2. Skip the pivot array
3. Read only from the slot array where actual VMA pointers are stored