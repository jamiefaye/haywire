# Finding Linux Kernel Structures in Physical Memory

## What We Need to Find

### 1. Process List (task_struct)
- Linux keeps all processes in a circular doubly-linked list
- Each task_struct contains:
  - `pid` - Process ID
  - `comm[16]` - Process name  
  - `mm` - Pointer to memory descriptor
  - `tasks` - List pointers to next/prev process

### 2. Memory Maps (mm_struct)
- Each process's mm_struct contains:
  - `mmap` - Pointer to first vm_area_struct
  - `pgd` - **Page Global Directory (this is TTBR!)**

### 3. Virtual Memory Areas (vm_area_struct)
- Linked list of memory regions:
  - `vm_start` - Start address
  - `vm_end` - End address  
  - `vm_flags` - Permissions
  - `vm_next` - Next VMA

## How to Find Without Agent

### Option 1: Signature Scanning
```c
// Look for "swapper/0" - the init process name
// Always PID 0, always at a fixed location
scan_for_bytes("swapper/0", 9);

// Once found, we have a task_struct
// Walk the linked list to find all processes
```

### Option 2: Known Symbols
```c
// The kernel symbol "init_task" points to PID 0
// Often at a predictable offset from kernel base
// Kernel base can be found by scanning for kernel signatures
```

### Option 3: KASLR Bypass
- Even with KASLR, certain structures are at fixed offsets
- System.map file has all symbols (if available)
- /proc/kallsyms exposes them (needs agent though)

## The Critical Find: init_task

Once we find init_task (PID 0), we can:
1. Walk the task list to find all processes
2. Read each process's mm->pgd to get its TTBR
3. Read the VMAs to get the memory map
4. Do page translation ourselves

## Signatures to Search For

### ARM64 Linux Kernel Signatures
- "Linux version" - Kernel version string
- "swapper/0" - Init process name
- ELF headers - Kernel usually has ELF signatures
- Exception vectors - Fixed patterns at known locations

### Physical Memory Layout (Typical)
- 0x00000000 - 0x40000000: Reserved/Device memory
- 0x40000000 - 0x80000000: **Kernel lives here**
- 0x80000000+: User memory

## Proof of Concept

```c
// Scan for init_task
for (addr = 0x40000000; addr < 0x80000000; addr += 8) {
    // Look for task_struct signature
    // - comm field contains "swapper/0"
    // - pid field is 0
    // - Reasonable pointer values
    
    if (matches_task_struct_pattern(addr)) {
        init_task = addr;
        break;
    }
}

// Walk the process list
current = init_task;
do {
    pid = read_u32(current + OFFSET_PID);
    name = read_string(current + OFFSET_COMM);
    pgd = read_u64(current + OFFSET_MM + OFFSET_PGD);
    
    printf("PID %d: %s, TTBR=0x%lx\n", pid, name, pgd);
    
    current = read_u64(current + OFFSET_TASKS_NEXT);
} while (current != init_task);
```

## The Reality

This is complex because:
1. Kernel struct offsets change between versions
2. KASLR randomizes addresses
3. Need to handle both ARM64 and x86_64
4. Struct layouts differ between configs

## Tools That Do This

- **Volatility** - Memory forensics framework
- **rekall** - Google's memory analysis
- **crash** - Red Hat's kernel crash analyzer

They all use "profiles" - pre-computed struct offsets for each kernel version.