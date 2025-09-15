#include "platform/x86_64/x86_64_page_walker.h"
#include "memory_backend.h"
#include <iostream>
#include <iomanip>
#include <cstring>

namespace Haywire {

X86_64PageWalker::X86_64PageWalker(MemoryBackend* backend)
    : PageWalker(backend), cr3(0), use5LevelPaging(false) {
}

X86_64PageWalker::~X86_64PageWalker() {
}

void X86_64PageWalker::SetPageTableBase(uint64_t cr3Val, uint64_t unused) {
    // CR3 contains the physical address of the PML4 (or PML5) table
    // Lower 12 bits are flags, clear them to get the base address
    cr3 = cr3Val & ~0xFFFULL;

    std::cerr << "x86-64 Page walker: CR3=0x" << std::hex << cr3 << std::dec << std::endl;

    // TODO: Detect 5-level paging from CR4.LA57 bit
    // For now, assume 4-level paging (standard for most systems)
    use5LevelPaging = false;
}

uint64_t X86_64PageWalker::TranslateAddress(uint64_t virtualAddr) {
    if (cr3 == 0) {
        return 0;
    }

    // Choose the appropriate translation based on paging mode
    if (use5LevelPaging) {
        return WalkPageTable5Level(virtualAddr);
    } else {
        return WalkPageTable4Level(virtualAddr);
    }
}

size_t X86_64PageWalker::TranslateRange(uint64_t startVA, size_t numPages,
                                        std::vector<uint64_t>& physAddrs) {
    physAddrs.clear();
    physAddrs.reserve(numPages);

    size_t successCount = 0;
    uint64_t va = startVA & ~PAGE_MASK;  // Align to page boundary

    for (size_t i = 0; i < numPages; i++, va += PAGE_SIZE) {
        uint64_t pa = TranslateAddress(va);
        physAddrs.push_back(pa);
        if (pa != 0) {
            successCount++;
        }
    }

    return successCount;
}

uint64_t X86_64PageWalker::WalkPageTable4Level(uint64_t va) {
    // 4-level page table walk for x86-64
    // VA layout: [47:39] PML4 | [38:30] PDPT | [29:21] PD | [20:12] PT | [11:0] offset

    uint64_t table_base = cr3;

    // Level 4: PML4 (Page Map Level 4)
    uint64_t pml4_index = (va >> PML4_SHIFT) & TABLE_MASK;
    uint64_t pml4_entry = ReadPhys64(table_base + pml4_index * 8);

    if (!(pml4_entry & PTE_PRESENT)) {
        return 0;  // Not present
    }

    // Level 3: PDPT (Page Directory Pointer Table)
    table_base = pml4_entry & PTE_ADDR_MASK;
    uint64_t pdpt_index = (va >> PDPT_SHIFT) & TABLE_MASK;
    uint64_t pdpt_entry = ReadPhys64(table_base + pdpt_index * 8);

    if (!(pdpt_entry & PTE_PRESENT)) {
        return 0;  // Not present
    }

    // Check for 1GB huge page
    if (pdpt_entry & PTE_PSE) {
        // 1GB page: bits [51:30] from PDPT entry + bits [29:0] from VA
        uint64_t page_base = pdpt_entry & 0x000FFFFFC0000000ULL;
        return page_base | (va & 0x3FFFFFFF);
    }

    // Level 2: PD (Page Directory)
    table_base = pdpt_entry & PTE_ADDR_MASK;
    uint64_t pd_index = (va >> PD_SHIFT) & TABLE_MASK;
    uint64_t pd_entry = ReadPhys64(table_base + pd_index * 8);

    if (!(pd_entry & PTE_PRESENT)) {
        return 0;  // Not present
    }

    // Check for 2MB large page
    if (pd_entry & PTE_PSE) {
        // 2MB page: bits [51:21] from PD entry + bits [20:0] from VA
        uint64_t page_base = pd_entry & 0x000FFFFFFFE00000ULL;
        return page_base | (va & 0x1FFFFF);
    }

    // Level 1: PT (Page Table)
    table_base = pd_entry & PTE_ADDR_MASK;
    uint64_t pt_index = (va >> PT_SHIFT) & TABLE_MASK;
    uint64_t pt_entry = ReadPhys64(table_base + pt_index * 8);

    if (!(pt_entry & PTE_PRESENT)) {
        return 0;  // Not present
    }

    // 4KB page: bits [51:12] from PT entry + bits [11:0] from VA
    uint64_t page_base = pt_entry & PTE_ADDR_MASK;
    return page_base | (va & PAGE_MASK);
}

uint64_t X86_64PageWalker::WalkPageTable5Level(uint64_t va) {
    // 5-level page table walk for x86-64 (LA57)
    // VA layout: [56:48] PML5 | [47:39] PML4 | [38:30] PDPT | [29:21] PD | [20:12] PT | [11:0] offset

    uint64_t table_base = cr3;

    // Level 5: PML5
    uint64_t pml5_index = (va >> PML5_SHIFT) & TABLE_MASK;
    uint64_t pml5_entry = ReadPhys64(table_base + pml5_index * 8);

    if (!(pml5_entry & PTE_PRESENT)) {
        return 0;  // Not present
    }

    // After PML5, continue with standard 4-level walk
    table_base = pml5_entry & PTE_ADDR_MASK;

    // Level 4: PML4
    uint64_t pml4_index = (va >> PML4_SHIFT) & TABLE_MASK;
    uint64_t pml4_entry = ReadPhys64(table_base + pml4_index * 8);

    if (!(pml4_entry & PTE_PRESENT)) {
        return 0;
    }

    // Continue with the rest of the 4-level walk...
    // (Implementation identical to WalkPageTable4Level from here)

    // For brevity, we'll reuse the logic
    // In production, you might want to factor out the common code
    return WalkPageTable4Level(va);
}

}