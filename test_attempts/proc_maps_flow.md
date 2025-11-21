# /proc/maps Data Flow

## 1. Iterator Setup (proc_maps_open)
```c
static int proc_maps_open(struct inode *inode, struct file *file)
{
    return do_maps_open(inode, file, &proc_pid_maps_op);
}
```

## 2. Sequence Operations (proc_pid_maps_op)
```c
const struct seq_operations proc_pid_maps_op = {
    .start  = m_start,
    .next   = m_next,
    .stop   = m_stop,
    .show   = show_map
};
```

## 3. Iterator Walk (m_start/m_next)
```c
static void *m_start(struct seq_file *m, loff_t *ppos)
{
    struct proc_maps_private *priv = m->private;
    struct mm_struct *mm = priv->mm;
    struct vm_area_struct *vma;

    // Initialize iterator
    vma_iter_init(&priv->iter, mm, *ppos);

    // Find first/next VMA
    vma = vma_next(&priv->iter);
    return vma;  // Returns vm_area_struct pointer directly!
}
```

## 4. VMA Iterator (vma_next)
```c
static inline struct vm_area_struct *vma_next(struct vma_iterator *vmi)
{
    return mas_find(&vmi->mas, ULONG_MAX);
}
```

## 5. Maple Tree Find (mas_find)
```c
void *mas_find(struct ma_state *mas, unsigned long max)
{
    // Walks maple tree and returns the VALUE stored in leaf node
    // For mm->mm_mt, this is a vm_area_struct pointer
    return entry;  // The actual vm_area_struct pointer
}
```

## 6. Show Function (show_map)
```c
static int show_map(struct seq_file *m, void *v)
{
    struct vm_area_struct *vma = v;  // Direct pointer, not double pointer!
    show_map_vma(m, vma);
    return 0;
}
```

## 7. Format Output (show_map_vma)
```c
static void show_map_vma(struct seq_file *m, struct vm_area_struct *vma)
{
    struct file *file = vma->vm_file;
    vm_flags_t flags = vma->vm_flags;

    // Format the output
    seq_printf(m, "%08lx-%08lx %c%c%c%c %08lx %02x:%02x %lu ",
               vma->vm_start,
               vma->vm_end,
               flags & VM_READ ? 'r' : '-',
               flags & VM_WRITE ? 'w' : '-',
               flags & VM_EXEC ? 'x' : '-',
               flags & VM_MAYSHARE ? 's' : 'p',
               vma->vm_pgoff << PAGE_SHIFT,
               ...);
}
```

## KEY INSIGHTS:

1. **The maple tree stores vm_area_struct pointers directly**
   - Not pointers to pointers
   - Not encoded values
   - Direct kernel virtual addresses pointing to vm_area_struct

2. **Leaf nodes contain the actual pointers**
   - When mas_find() reaches a leaf, it returns the value stored there
   - This value IS the vm_area_struct pointer

3. **No additional dereferencing needed**
   - The iterator returns vma pointer
   - show_map receives vma pointer
   - Directly accesses vma->vm_start, vma->vm_end, etc.

## What This Means for Our Maple Tree Walker:

The values we see in leaf nodes that start with 0xffff... SHOULD be vm_area_struct pointers.
When we read them and they show vm_start=0x1, vm_end=0x0, this means:

1. We're reading at the wrong offset in the struct, OR
2. The pointer is stale/freed, OR
3. We're misinterpreting the maple tree structure

The strange values like 0xea6f57989000 might be:
- Keys (address ranges) stored alongside values
- Metadata or pivots for the maple tree
- Not actual slot data but range markers