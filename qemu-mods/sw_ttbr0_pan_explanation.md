# ARM64_SW_TTBR0_PAN Explanation

## How it works:
1. **In kernel mode**: TTBR0 = reserved empty table (0x136dbe000)
2. **In user mode**: TTBR0 = actual process page table
3. **TTBR1**: Always contains kernel page tables (0x136dbf001) - shared by all

## Why we can't track processes by TTBR:
- When we sample from QEMU, CPUs are usually in kernel mode
- In kernel mode, TTBR0 always shows the reserved table
- TTBR1 is shared across all processes (kernel mappings)
- User page tables are only loaded into TTBR0 during user execution

## The actual process page tables:
- Stored in task_struct->mm->pgd (page global directory)
- Only loaded into TTBR0 when switching to user mode
- Immediately replaced with reserved table when entering kernel

## How to track processes:
1. Walk kernel task list from init_task
2. Read task_struct->mm to get memory descriptor
3. Read mm_struct->pgd to get actual page table address
4. Use task_struct->pid and ->comm for process info

This is a security feature to prevent kernel from accidentally accessing user memory.