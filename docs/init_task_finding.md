# Finding init_task (Process List Head) in Physical Memory

## The Good News: init_task Symbol

The process list head is called `init_task` in Linux. It represents PID 0 (swapper/idle process).

## Location Strategies

### 1. WITHOUT KASLR (Kernel Address Space Layout Randomization)
- **Fixed virtual address** from System.map
- Example: `ffffffff82a11780 D init_task`
- Physical = Virtual - PAGE_OFFSET
- On ARM64: PAGE_OFFSET = 0xffff000000000000 typically

### 2. WITH KASLR (Most Modern Systems)
- Base address is randomized at boot
- BUT: Offset from kernel base is constant for a given build
- Strategy: Find kernel base first, then add offset

### 3. Kernel Base Detection
```c
// Method A: Search for kernel signature
// The kernel starts with specific patterns
uint64_t find_kernel_base() {
    // Look for "Linux version" string
    // Look for __start_kernel symbol patterns
    // Look for exception vector table (ARM64)
    
    for (addr = 0x40000000; addr < 0x80000000; addr += 0x200000) {
        if (looks_like_kernel_start(addr)) {
            return addr;
        }
    }
}

// Method B: Find from known kernel symbols
// _text, _stext, or startup_64/startup_32
```

### 4. Physical Memory Layout (ARM64 Linux)

```
Physical Memory:
0x40000000: [kernel .text starts here often]
0x40100000: [kernel .data]
...
0x40xxxxxx: [init_task lives in .data section]
```

## Real Example from Linux Source

```c
// init/init_task.c
struct task_struct init_task = {
    .comm = "swapper",
    .pid = 0,
    .tasks = LIST_HEAD_INIT(init_task.tasks),
    ...
};
```

## Finding init_task Without Symbols

### Signature-Based Search
```c
struct task_struct_signature {
    // Look for these patterns
    uint32_t pid;           // = 0 for init_task
    char comm[16];          // = "swapper" or "swapper/0"
    // Pointers that look valid (kernel addresses)
    uint64_t tasks_next;    // Points to valid kernel address
    uint64_t tasks_prev;    // Points to valid kernel address
};

uint64_t find_init_task() {
    for (addr = 0x40000000; addr < 0x80000000; addr += 8) {
        if (read_u32(addr + PID_OFFSET) == 0) {
            char comm[16];
            read_bytes(addr + COMM_OFFSET, comm, 16);
            if (strncmp(comm, "swapper", 7) == 0) {
                // Verify it's really a task_struct
                uint64_t next = read_u64(addr + TASKS_NEXT_OFFSET);
                if (next > 0xffff000000000000 && next < 0xffffffffffffffff) {
                    return addr;  // Found it!
                }
            }
        }
    }
}
```

## The Offset Problem

Different kernel configs/versions have different offsets:

```c
// Kernel 5.x typical offsets (ARM64)
#define OFFSET_PID    0x398
#define OFFSET_COMM   0x550
#define OFFSET_TASKS  0x2F8
#define OFFSET_MM     0x3A0

// Kernel 6.x might be different!
```

## Practical Approach for Haywire

1. **Build a profile** for common Ubuntu ARM64 kernels:
   ```c
   struct kernel_profile {
       const char* version;
       uint64_t init_task_offset;  // Offset from kernel base
       uint32_t pid_offset;
       uint32_t comm_offset;
       uint32_t mm_offset;
       uint32_t pgd_offset;
   };
   ```

2. **Detect kernel version** from version string in memory

3. **Use profile to find structures**

## Or... Hybrid Approach

Use agent ONCE at startup to get:
- `/proc/kallsyms` - All kernel symbols including init_task
- `/boot/System.map` - Symbol addresses
- `cat /proc/1/maps | grep kernel` - Kernel base address

Cache these and then go agent-free!

## Bottom Line

- init_task IS at a fixed offset from kernel base (per build)
- Kernel base moves with KASLR but can be detected
- We can find it with signatures but need version-specific offsets
- **Most practical**: Use agent once to bootstrap, then go direct