
  1. task_struct offsets (Process information)

  - pid         // Process ID
  - tgid        // Thread group ID
  - comm[16]    // Process name
  - mm          // Pointer to mm_struct
  - tasks       // Linked list of all tasks
  - active_mm   // Active memory descriptor
  - real_parent // Parent process
  - children    // Child processes list

  2. mm_struct offsets (Memory management)

  - mm_count    // Primary reference count (0x00)
  - mm_mt       // Maple tree structure (0x40)
  - mm_mt.ma_root // Actual maple tree root pointer (0x48)
  - pgd         // Page Global Directory pointer (0x68)
  - mm_users    // Active user count (0x74) - NOT 0x38!
  - mmap        // VMA linked list (pre-6.1 kernels)
  - start_code  // Code segment start
  - end_code    // Code segment end
  - start_data  // Data segment start
  - end_data    // Data segment end
  - start_brk   // Heap start
  - brk         // Current heap end
  - start_stack // Stack start

  3. vm_area_struct offsets (Virtual Memory Areas)

  - vm_start    // VMA start address (0x00)
  - vm_end      // VMA end address (0x08)
  - vm_next     // Next VMA in list (varies)
  - vm_flags    // Permissions (0x20) - 32 decimal
  - vm_pgoff    // Page offset in file (0x78) - 120 decimal
  - vm_file     // Backing file (0x80) - 128 decimal

  4. struct file offsets

  - f_op        // File operations (0x10) - 16 decimal
  - f_inode     // Inode pointer (0x28) - 40 decimal
  - f_path      // Path structure (0x40) - 64 decimal

  5. struct path offsets

  - mnt         // VFS mount (0x00)
  - dentry      // Directory entry (0x08)

  6. struct dentry offsets

  - d_name      // Name (qstr) (0x20) - 32 decimal

  7. struct qstr offsets

  - hash/len    // Hash and length (0x00-0x07)
  - name        // Name pointer (0x08)

  8. Page table offsets (Architecture-specific)

  For ARM64:
  - PGD_SHIFT   // Bits for PGD index (usually 39)
  - PUD_SHIFT   // Bits for PUD index (usually 30)
  - PMD_SHIFT   // Bits for PMD index (usually 21)
  - PAGE_SHIFT  // Page size bits (12 for 4KB)

  5. Special symbols/addresses

  - init_task   // First process (PID 0)
  - swapper_pg_dir // Kernel's PGD

  How These Vary:

  By Kernel Version:

  - Pre-6.1: VMAs in linked list (mmap field)
  - 6.1+: VMAs in maple tree (mm_mt field)
  - Structure sizes change with added/removed fields
  - Field order can change between major versions

  By Architecture:

  - x86_64 vs ARM64 vs RISC-V: Different page table layouts
  - Register sizes (32 vs 64 bit)
  - Endianness (though most are little-endian now)

  By Kernel Config:

  - CONFIG_PREEMPT: Adds preemption counters
  - CONFIG_MEMCG: Adds memory cgroup fields
  - CONFIG_NUMA: Adds NUMA-related fields
  - CONFIG_KASAN: Changes structure padding
  - CONFIG_DEBUG_*: Adds debug fields

  Methods to Find Offsets:

  1. BTF (Best - if available)

  pahole -C task_struct
  pahole -C mm_struct

  2. System.map / kallsyms

  grep init_task /proc/kallsyms
  grep swapper_pg_dir /proc/kallsyms

  3. Debug info from vmlinux

  gdb vmlinux
  (gdb) ptype struct task_struct
  (gdb) print &((struct task_struct *)0)->pid

  4. Runtime detection

  - Find known process (PID 1)
  - Search for known values
  - Validate structure patterns

  5. Heuristics

  - PIDs are usually 4-byte aligned
  - Pointers have kernel address patterns (0xffff...)
  - Names are null-terminated ASCII
  - Linked lists have forward/back pointers

⏺ For Haywire to be portable, you'll need:

  1. Offset configuration files per kernel version
  2. Auto-detection logic using heuristics
  3. BTF parsing when available (most modern kernels)
  4. Fallback patterns for unknown kernels

  The most critical ones for basic functionality:
  - task_struct.mm - to get mm_struct
  - mm_struct.pgd - to get page tables
  - mm_struct.mm_mt or .mmap - to get VMAs
  - task_struct.comm - to identify processes
  - task_struct.pid - for process IDs