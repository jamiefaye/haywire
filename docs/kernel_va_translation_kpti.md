# Kernel VA Translation and KPTI Discovery

## Date: September 22, 2024

## The Problem

We were trying to implement kernel discovery in JavaScript/TypeScript to match our working Python implementation that successfully finds 1010 task_structs. The core challenge was translating kernel virtual addresses (VAs) to physical addresses (PAs) to extract PGD pointers from mm_structs.

### Initial Symptoms
- Found 217 PGD candidates but none validated
- All kernel addresses (0xffff0000...) were calculating to PGD index 0
- PGD entries at calculated indices appeared invalid
- Could not translate any kernel VAs to PAs

## The Investigation

### Step 1: Understanding the Address Space
For ARM64 Linux with 48-bit virtual addressing:
- User space: `0x0000000000000000` to `0x0000ffffffffffff`
- Kernel space: `0xffff000000000000` to `0xffffffffffffffff`

### Step 2: Page Table Index Calculation
For a kernel address like `0xffff0000c557d000`:
```
VA bits [47:39] → PGD index (9 bits = 512 entries)
VA bits [38:30] → PUD index
VA bits [29:21] → PMD index
VA bits [20:12] → PTE index
VA bits [11:0]  → page offset
```

When we shifted `0xffff0000c557d000` right by 39 bits:
- Result: `0x1fffe00`
- Masked with 0x1FF: `0x000` (index 0)

This seemed wrong - shouldn't kernel addresses use the upper half of the PGD table?

### Step 3: The Failed Attempts
We tried several approaches:
1. **Adding 256 to kernel PGD indices** - Didn't work, entries at 256+ were also invalid
2. **Treating as 32-bit kernel VAs** - Misunderstood the architecture
3. **Looking for unified PGD** - Found candidates but none validated

## The Breakthrough: KPTI (Kernel Page Table Isolation)

The key insight came from understanding **KPTI (Kernel Page Table Isolation)**, also known as KAISER, which was implemented to mitigate Meltdown vulnerabilities.

### How KPTI Works
With KPTI, Linux uses **SEPARATE** page tables for security:

1. **User PGD** (per-process, in mm_struct)
   - Maps user space addresses
   - Contains MINIMAL kernel mappings (only trampoline code for syscalls)
   - Used during user mode execution (TTBR0_EL1)

2. **Kernel PGD** (swapper_pg_dir, shared)
   - Maps ALL kernel space
   - Also maps user space for the current process
   - Used during kernel mode execution (TTBR1_EL1)

### Why This Matters
- When a syscall happens, CPU switches from user PGD to kernel PGD
- Process mm_struct points to the USER PGD (limited kernel visibility)
- Kernel VAs can ONLY be translated using swapper_pg_dir
- This is a security feature - prevents Meltdown attacks

## The Solution

### Finding the Real swapper_pg_dir
Instead of trying to extract PGDs from mm_structs and validate them, we:
1. Searched all memory for PGD-like structures (found 217 candidates)
2. Tested each candidate to see if it could translate kernel VAs
3. Found that `0x613cd000` successfully translated all kernel mm_struct addresses

### Validation Test
```javascript
// For each PGD candidate, try to translate known kernel VAs
for (const process of processes) {
    if (process.mmStruct > 0xffff000000000000) {
        const pa = translateVA(process.mmStruct, candidatePGD);
        if (pa && isValidPA(pa)) {
            // This PGD can translate kernel addresses!
            successCount++;
        }
    }
}
```

The winning PGD at `0x613cd000` successfully translated 5/5 kernel VAs, confirming it as the real swapper_pg_dir.

## Key Learnings

1. **Modern Linux uses KPTI for security** - Always assume separate user/kernel page tables post-2018

2. **PGD index 0 was correct** - For address `0xffff0000c557d000`, index 0 IS the right index in the kernel PGD

3. **Two different PGD contexts**:
   - User PGD: What's in mm_struct, can't translate most kernel VAs
   - Kernel PGD: swapper_pg_dir, can translate all kernel VAs

4. **Validation approach**: Don't try to follow pointers from mm_struct to find kernel PGD. Instead, scan memory for PGD candidates and test which one successfully translates kernel addresses.

## Implementation Notes

### Correct Page Table Walk for ARM64
```javascript
// For 48-bit ARM64 with 4KB pages
const pgdIndex = (virtualAddr >> 39n) & 0x1FFn;  // bits 47-39
const pudIndex = (virtualAddr >> 30n) & 0x1FFn;  // bits 38-30
const pmdIndex = (virtualAddr >> 21n) & 0x1FFn;  // bits 29-21
const pteIndex = (virtualAddr >> 12n) & 0x1FFn;  // bits 20-12
const offset   = virtualAddr & 0xFFFn;           // bits 11-0
```

### Detecting PGDs
Look for 4KB-aligned pages with:
- Valid page table entries (bits [1:0] = 1 or 3)
- Entries pointing to reasonable physical addresses
- Successful pointer chain validation (PGD→PUD→PMD→PTE)

### KPTI Detection
If you find that:
- Process PGDs can't translate kernel VAs
- A separate PGD can translate all kernel VAs
- The kernel PGD has full kernel mappings

Then KPTI is active on the system.

## Security Implications

KPTI is a critical security feature that:
- Prevents user processes from reading kernel memory
- Mitigates Meltdown vulnerability (CVE-2017-5754)
- Adds overhead due to page table switches on syscalls
- Is enabled by default on most modern Linux distributions

Understanding KPTI is essential for:
- Kernel debugging and introspection tools
- Security research
- Performance analysis
- Virtual machine introspection

## References
- [KPTI Documentation](https://www.kernel.org/doc/html/latest/arch/x86/pti.html)
- [ARM64 Memory Management](https://www.kernel.org/doc/html/latest/arm64/memory.html)
- [Meltdown Paper](https://meltdownattack.com/meltdown.pdf)