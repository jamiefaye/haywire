# VA Mode Debugging Session - October 29, 2025

## Summary

VA mode is failing because we're finding **stale/freed task_structs** during memory scanning. Their mm pointers point to freed memory containing garbage, causing 0/141 success rate for PGD extraction.

## What We Discovered Today

### 1. Process Discovery Works
- Successfully finding 192 processes by memory scanning
- Process names are correct (nautilus, gpg-connect-age, Firefox, etc.)
- task_struct offsets from profile are correct (pid, comm, mm)

### 2. KASLR Base Detection Works
- Successfully detected actual KASLR direct map base: **`0xffff8b7400000000`**
- Method: Sampled 20 mm_struct pointers from discovered processes
- Used 4GB alignment mask (`0xFFFFFFFF00000000`) to extract base
- All 20 samples agreed on same base

### 3. The Real Problem: Stale task_structs

**Memory scan finds BOTH:**
- ✅ Active processes (currently running)
- ❌ Freed processes (exited but memory not zeroed)

**Evidence:**
```
PID 50 (png):
  mm VA: 0xffff8b7452f07600
  mm PA: 0x52f07600 (with correct KASLR base)
  mm_users: 1344683890 ← GARBAGE (should be 1-10,000)
  pgd: 0x5fb0da34ab5c2ca6 ← GARBAGE (not a kernel pointer)

PID 1070 (dconf worker):
  mm VA: 0xffff8b7440080580
  mm PA: 0x40080580 (with correct KASLR base)
  mm_users: 2937517342 ← GARBAGE
  pgd: 0x6db9ddc838d25f58 ← GARBAGE
```

The mm pointers in these task_structs point to **freed memory** that hasn't been overwritten yet.

### 4. Why Memory Scan Fails

**Kernel memory allocation:**
- When a process exits, its task_struct is freed
- But the memory isn't zeroed immediately
- Memory scan finds these old structures that look valid:
  - PID field still has valid number
  - comm field still has valid process name
  - mm pointer still looks like a kernel address
- But following the mm pointer leads to garbage

**Result:** 141 user processes found, 0 have valid mm_structs

### 5. What Works

**Profile System:**
- ✅ Kernel profile loading works (ubuntu-22.04-x86_64.json)
- ✅ Offsets are correct (verified with pahole):
  - task_struct.mm: 0x938 (2360)
  - mm_struct.pgd: 0x80 (128)
  - mm_struct.mm_users: 0x8c (140)

**KASLR Detection:**
- ✅ Automatic detection from mm pointers works
- ✅ Detects `0xffff8b7400000000` with 100% agreement

**Translation Logic:**
- ✅ TranslateKernelVA() works correctly
- ✅ Uses detected page_offset_base
- ✅ Produces valid physical addresses (1-2GB range in 4GB VM)

## Root Cause

**The fundamental issue:** Memory scanning cannot distinguish between active and freed processes.

## Solution: Walk Task List Instead

### What We Need

Instead of scanning all memory, **walk the linked list** starting from `init_task`:

```
init_task (PID 0)
  ↓ tasks.next
task_struct (PID 1: systemd)
  ↓ tasks.next
task_struct (PID 2: kthreadd)
  ↓ tasks.next
... (all active processes)
```

This list **only contains active processes** - freed processes are removed from the list.

### Implementation Plan

**1. Get init_task address** ✅ DONE
```
From VM: sudo grep " init_task$" /proc/kallsyms
Result: 0xffffffff9ea0fcc0
```

**2. Translate to Physical Address**
- Use swapper PGD to translate `0xffffffff9ea0fcc0` → PA
- This should work since it's in kernel text/data region

**3. Walk the List**
```cpp
bool WalkTaskList(uint64_t initTaskPA) {
    uint64_t currentPA = initTaskPA;
    std::set<uint64_t> visited;  // Prevent cycles

    while (currentPA != 0 && visited.find(currentPA) == visited.end()) {
        visited.insert(currentPA);

        // Read task_struct at currentPA
        uint8_t* task = (uint8_t*)memBase + (currentPA - RAM_BASE);

        // Extract process info
        ProcessInfo proc;
        proc.pid = *(uint32_t*)(task + kernelInfo.offsets.pid);
        memcpy(proc.comm, task + kernelInfo.offsets.comm, 16);
        proc.mm_addr = *(uint64_t*)(task + kernelInfo.offsets.mm);

        processes.push_back(proc);

        // Follow tasks.next pointer
        uint64_t nextTaskVA = *(uint64_t*)(task + kernelInfo.offsets.tasks_list);

        // Convert back to task_struct address (list entry is embedded)
        uint64_t nextTaskStructVA = nextTaskVA - kernelInfo.offsets.tasks_list;

        // Translate to PA for next iteration
        currentPA = TranslateVA(nextTaskStructVA, kernelInfo.swapper_pgd);

        // Stop if we've circled back to init_task
        if (currentPA == initTaskPA) break;
    }

    return !processes.empty();
}
```

**Key advantage:** All processes found this way are **guaranteed active** because only active processes are in the linked list.

## Tomorrow's Tasks

### 1. Implement WalkTaskList() Function
- **File:** `src/kernel_discovery.cpp`
- **Location:** Add as private method in KernelDiscovery class
- **Test:** Should find ~141 active processes

### 2. Modify DiscoverProcesses()
- Try WalkTaskList first
- Fall back to memory scan only if walk fails
- **Expected result:** 100+ PGD extractions succeed

### 3. Verify Results
- Run haywire and check bug.log
- Should see: "Extracted PGDs: 100+ success, few/no failed"
- PIDs should match /proc in VM

### 4. Test VA Mode
- Select a process in PID selector
- Verify memory rendering works
- Check if we can see process memory correctly

## Files Modified Today

### src/kernel_discovery.cpp
- Added `page_offset_base` field to `KernelInfo` struct
- Added KASLR base detection from mm pointers (4GB alignment)
- Added mm_struct validation with debug output
- **Status:** Detection works, but using stale processes

### profiles/ubuntu-22.04-x86_64.json
- Updated offsets based on pahole output from VM
- mm_struct.pgd: 128 (was 104)
- mm_struct.mm_users: 140 (was 116)
- vm_area_struct.vm_file: 136 (was 128)
- **Status:** Offsets confirmed correct

## Key Insights

1. **Memory scan is fundamentally flawed** for process discovery - it finds too many false positives (stale processes)

2. **Linked list walk is the correct approach** - it's how the kernel tracks active processes

3. **KASLR detection works perfectly** - we can reliably detect the direct map base from a sample of kernel pointers

4. **Profile offsets are correct** - verified with pahole, matches actual kernel structures

5. **x86_64 RAM starts at PA 0x0** - unlike ARM64 which starts at 0x40000000

## Questions for Tomorrow

1. Can we translate kernel text addresses (like init_task) with swapper PGD?
   - init_task is at `0xffffffff9ea0fcc0` (kernel text region)
   - May need different translation method than direct map

2. Should we cache the active process list?
   - Task list walk is faster than memory scan
   - But need to handle new processes spawning

3. Do we need QMP for init_task translation?
   - Or can swapper PGD translate kernel text region?

## References

- **Kernel profile:** `profiles/ubuntu-22.04-x86_64.json`
- **Init task symbol:** `0xffffffff9ea0fcc0` (from VM kallsyms)
- **KASLR base:** `0xffff8b7400000000` (detected)
- **Swapper PGD:** `0xf5917000` or similar (varies)

## Commands for Tomorrow

**In VM (if needed):**
```bash
# Verify init_task address
sudo grep " init_task$" /proc/kallsyms

# Get current process count for comparison
ps aux | wc -l

# Check pahole for any missing offsets
sudo pahole -C task_struct /sys/kernel/btf/vmlinux | grep tasks
```

**In Haywire:**
```bash
# Build
cd /mnt/c/Users/jamie/haywire
cmake --build build -j8

# Run and check output
build/haywire 2>&1 | tee build/bug.log

# Check results
grep "Extracted PGDs" build/bug.log
grep "Found.*active processes" build/bug.log
```
