# Process List → Memory Info Chain

## From task_struct, we get EVERYTHING:

```c
struct task_struct {  // Each process
    // ... other fields ...
    
    struct mm_struct *mm;  // Memory descriptor - THIS IS GOLD!
    
    // ... more fields ...
    char comm[16];         // Process name
    pid_t pid;             // Process ID
    struct list_head tasks; // Links to other processes
};

struct mm_struct {  // Memory descriptor
    struct vm_area_struct *mmap;  // List of memory regions!
    pgd_t *pgd;                    // PAGE TABLE BASE (THIS IS TTBR!)
    
    unsigned long start_code, end_code;  // Code segment
    unsigned long start_data, end_data;  // Data segment  
    unsigned long start_brk, brk;        // Heap
    unsigned long start_stack;           // Stack
    
    unsigned long total_vm;    // Total pages mapped
    unsigned long locked_vm;   // Pages locked in memory
    // ... more stats ...
};

struct vm_area_struct {  // Each memory region (like /proc/pid/maps)
    unsigned long vm_start;     // Start address
    unsigned long vm_end;       // End address
    unsigned long vm_flags;     // Permissions (R/W/X)
    
    struct vm_area_struct *vm_next;  // Next region
    
    struct file *vm_file;       // Mapped file (if any)
    unsigned long vm_pgoff;     // File offset
};
```

## The Complete Chain:

1. **Find init_task** (process list head)
   ↓
2. **Walk task_struct list** → Get all processes
   ↓
3. **For each process, read mm pointer** → Get memory descriptor
   ↓
4. **From mm_struct:**
   - **pgd** = Page Global Directory = **TTBR!** ✓
   - **mmap** = List of memory regions = **/proc/pid/maps!** ✓
   ↓
5. **Walk vm_area_struct list** → Get all memory regions
   ↓
6. **Now we have everything:**
   - Process list ✓
   - Memory maps ✓  
   - TTBR for page translation ✓

## No Agent Needed!

```c
// Pseudo-code for agent-free operation
void read_all_process_info() {
    // Start from init_task (found via scanning)
    task_struct* current = init_task;
    
    do {
        // Read process info
        pid = current->pid;
        name = current->comm;
        
        // Get memory info
        mm_struct* mm = current->mm;
        if (mm) {
            // THIS IS THE TTBR!
            uint64_t ttbr = mm->pgd;
            
            // Walk memory regions (like /proc/pid/maps)
            vm_area_struct* vma = mm->mmap;
            while (vma) {
                printf("%lx-%lx %c%c%c %s\n", 
                    vma->vm_start, vma->vm_end,
                    (vma->vm_flags & VM_READ) ? 'r' : '-',
                    (vma->vm_flags & VM_WRITE) ? 'w' : '-',
                    (vma->vm_flags & VM_EXEC) ? 'x' : '-',
                    vma->vm_file ? get_filename(vma->vm_file) : "");
                
                vma = vma->vm_next;
            }
        }
        
        current = next_task(current);
    } while (current != init_task);
}
```

## The Magic Field: mm->pgd

**pgd (Page Global Directory) IS the TTBR!**

- On ARM64: `mm->pgd` points to the page table base
- This is exactly what gets loaded into TTBR0_EL1
- We can use this directly with our page walker!

## What We Can Do:

1. **Process enumeration** - Walk task list
2. **Memory maps** - Walk VMA list  
3. **Page translation** - Use mm->pgd as TTBR
4. **Memory reading** - Translate VA→PA, read physical

All without the guest agent!

## The Catch:

We need kernel version-specific offsets:
```c
// These offsets change between kernel versions
#define TASK_MM_OFFSET     0x3A0  // task_struct.mm
#define MM_PGD_OFFSET      0x48   // mm_struct.pgd  
#define MM_MMAP_OFFSET     0x00   // mm_struct.mmap
#define VMA_START_OFFSET   0x00   // vm_area_struct.vm_start
#define VMA_END_OFFSET     0x08   // vm_area_struct.vm_end
#define VMA_NEXT_OFFSET    0x18   // vm_area_struct.vm_next
```

But once we have these (from System.map or testing), we're agent-free!