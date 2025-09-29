# Exact /proc/maps Implementation Trace

## 1. Entry Point: `/proc/PID/maps`
```c
// fs/proc/base.c
static const struct file_operations proc_pid_maps_operations = {
    .open    = proc_pid_maps_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = proc_map_release,
};
```

## 2. Open Function
```c
static int proc_pid_maps_open(struct inode *inode, struct file *file)
{
    return do_maps_open(inode, file, &proc_pid_maps_op);
}

static int do_maps_open(struct inode *inode, struct file *file,
                       const struct seq_operations *ops)
{
    struct proc_maps_private *priv;
    priv = __seq_open_private(file, ops, sizeof(*priv));

    // Get the task
    struct task_struct *task = get_proc_task(inode);

    // Get the mm_struct
    struct mm_struct *mm = mm_for_maps(task);

    priv->mm = mm;
    priv->task = task;

    // Initialize the VMA iterator with the mm_struct
    return 0;
}
```

## 3. Critical: Getting mm_struct
```c
static struct mm_struct *mm_for_maps(struct task_struct *task)
{
    struct mm_struct *mm;

    // Get mm with proper locking
    task_lock(task);
    mm = task->mm;  // NOT task->active_mm!

    if (mm) {
        // Check if we can access it
        if (task->mm != current->mm)
            // Check permissions...

        mmget(mm);  // Increment mm_users!
    }
    task_unlock(task);

    return mm;
}
```

## 4. The Iterator Setup
```c
static void *m_start(struct seq_file *m, loff_t *ppos)
{
    struct proc_maps_private *priv = m->private;
    struct mm_struct *mm = priv->mm;
    struct vm_area_struct *vma;

    // Lock the mm
    mmap_read_lock(mm);

    // Initialize VMA iterator
    vma_iter_init(&priv->iter, mm, *ppos);

    // Find first VMA
    vma = vma_next(&priv->iter);

    return vma;
}
```

## 5. VMA Iterator Initialization
```c
static inline void vma_iter_init(struct vma_iterator *vmi,
                                 struct mm_struct *mm,
                                 unsigned long addr)
{
    mas_init(&vmi->mas, &mm->mm_mt, addr);
}

void mas_init(struct ma_state *mas, struct maple_tree *tree,
              unsigned long addr)
{
    memset(mas, 0, sizeof(struct ma_state));
    mas->tree = tree;
    mas->index = addr;
    mas->last = ULONG_MAX;
    mas->node = MAS_START;
}
```

## 6. THE CRITICAL PART: Finding VMAs
```c
static inline struct vm_area_struct *vma_next(struct vma_iterator *vmi)
{
    return mas_find(&vmi->mas, ULONG_MAX);
}

void *mas_find(struct ma_state *mas, unsigned long max)
{
    if (unlikely(mas_is_start(mas))) {
        // First time - initialize
        if (mas->index > max)
            return NULL;

        // Start from the root
        mas->node = mas->tree->ma_root;
    }

    // Walk the maple tree
    return mas_next_entry(mas, max);
}
```

## KEY INSIGHTS:

### 1. mm_users vs mm_count
- `mm_users`: Number of processes using this mm (threads share mm)
- `mm_count`: Reference count (always >= 1 if mm exists)
- When process exits: mm_users goes to 0, but mm_count may still be > 0
- **/proc/maps actually increments mm_users when it opens!**

### 2. The Maple Tree Root
```c
struct mm_struct {
    struct {
        struct maple_tree mm_mt;  // At specific offset
        // ...
    };
    // ...
};

struct maple_tree {
    union {
        spinlock_t ma_lock;
        lockdep_map_p mt_lock;
    };
    void __rcu *ma_root;  // The actual root pointer
    unsigned int ma_flags;
};
```

### 3. Special Root Encodings
The ma_root can be:
- `NULL` (0) - Empty tree
- `MAS_ROOT` (0x1) - Special marker
- `MAS_NONE` (0x2) - No entry
- Actual node pointer with type in low bits

### 4. What We're Missing

1. **mm_users = 0 doesn't mean no VMAs!**
   - The VMAs might still be there
   - /proc/maps works even on exiting processes
   - It just means no *user* threads are using it

2. **The maple tree might be in a special state**
   - Check for special root values (0x1, 0x2, etc.)
   - Single VMA optimization (root points directly to VMA)

3. **We need to handle special encodings**
   - `mte_to_node()` cleans pointers: `(void *)((unsigned long)entry & ~MAPLE_NODE_MASK)`
   - `MAPLE_NODE_MASK` is 0xFF

## Testing Steps:

1. Check if ma_root is a special value (0x1, 0x2, etc.)
2. Check if it's a direct VMA pointer (single VMA case)
3. Check if the tree is empty vs corrupted

## The Real Question:

**Why does /proc/maps see VMAs when mm_users = 0?**

Answer: Because the VMAs are still there! The kernel doesn't immediately free them when mm_users hits 0. The mm_struct and its maple tree remain valid until mm_count hits 0.

## What We Should Do:

1. **Ignore mm_users = 0** - Try to walk the maple tree anyway
2. **Check for special root encodings** - Handle MAS_ROOT, MAS_NONE
3. **Look for direct VMA pointers** - Single VMA optimization
4. **Verify our maple tree offset** - Make sure we're reading the right field