#include "platform/arm64/arm64_page_walker.h"
#include "memory_backend.h"
#include <iostream>
#include <iomanip>
#include <cstring>

namespace Haywire {

ARM64PageWalker::ARM64PageWalker(MemoryBackend* backend)
    : PageWalker(backend), ttbr0(0), ttbr1(0) {
}

ARM64PageWalker::~ARM64PageWalker() {
}

void ARM64PageWalker::SetPageTableBase(uint64_t ttbr0Val, uint64_t ttbr1Val) {
    ttbr0 = ttbr0Val & ~0xFFFULL;  // Clear lower bits
    ttbr1 = ttbr1Val & ~0xFFFULL;

    std::cerr << "ARM64 Page walker: TTBR0=0x" << std::hex << ttbr0
              << " TTBR1=0x" << ttbr1 << std::dec << std::endl;
}

uint64_t ARM64PageWalker::WalkPageTable(uint64_t va, uint64_t ttbr) {
    if (ttbr == 0) {
        return 0;
    }

    uint64_t table_base = ttbr;

    // Level 0 lookup (bits 47:39)
    uint64_t l0_index = (va >> L0_SHIFT) & TABLE_MASK;
    uint64_t l0_pte = ReadPhys64(table_base + l0_index * 8);

    if (!(l0_pte & DESC_VALID)) {
        return 0;  // Not mapped
    }

    // Level 1 lookup (bits 38:30)
    table_base = l0_pte & ~0xFFFULL;
    uint64_t l1_index = (va >> L1_SHIFT) & TABLE_MASK;
    uint64_t l1_pte = ReadPhys64(table_base + l1_index * 8);

    if (!(l1_pte & DESC_VALID)) {
        return 0;
    }

    // Check for 1GB huge page
    if (!(l1_pte & DESC_TABLE)) {
        // 1GB page - bits 29:0 from VA
        return (l1_pte & ~0x3FFFFFFFULL) | (va & 0x3FFFFFFFULL);
    }

    // Level 2 lookup (bits 29:21)
    table_base = l1_pte & ~0xFFFULL;
    uint64_t l2_index = (va >> L2_SHIFT) & TABLE_MASK;
    uint64_t l2_pte = ReadPhys64(table_base + l2_index * 8);

    if (!(l2_pte & DESC_VALID)) {
        return 0;
    }

    // Check for 2MB huge page
    if (!(l2_pte & DESC_TABLE)) {
        // 2MB page - bits 20:0 from VA
        return (l2_pte & ~0x1FFFFFULL) | (va & 0x1FFFFFULL);
    }

    // Level 3 lookup (bits 20:12)
    table_base = l2_pte & ~0xFFFULL;
    uint64_t l3_index = (va >> L3_SHIFT) & TABLE_MASK;
    uint64_t l3_pte = ReadPhys64(table_base + l3_index * 8);

    if (!(l3_pte & DESC_VALID)) {
        return 0;
    }

    // 4KB page - bits 11:0 from VA
    return (l3_pte & ~0xFFFULL) | (va & 0xFFFULL);
}

uint64_t ARM64PageWalker::TranslateAddress(uint64_t virtualAddr) {
    // Determine which TTBR to use based on VA bit 47
    // (simplified - real logic depends on TCR_EL1 settings)
    uint64_t ttbr = (virtualAddr & (1ULL << 47)) ? ttbr1 : ttbr0;

    return WalkPageTable(virtualAddr, ttbr);
}

size_t ARM64PageWalker::TranslateRange(uint64_t startVA, size_t numPages,
                                       std::vector<uint64_t>& physAddrs) {
    physAddrs.clear();
    physAddrs.reserve(numPages);

    size_t successCount = 0;
    for (size_t i = 0; i < numPages; i++) {
        uint64_t va = startVA + i * PAGE_SIZE;
        uint64_t pa = TranslateAddress(va);
        physAddrs.push_back(pa);
        if (pa != 0) {
            successCount++;
        }
    }

    return successCount;
}

void ARM64PageWalker::DumpMappings(uint64_t maxVA) {
    std::cout << "Dumping ARM64 page mappings up to 0x" << std::hex << maxVA << std::dec << "\n";

    size_t mapped = 0;
    size_t unmapped = 0;

    for (uint64_t va = 0; va < maxVA; va += PAGE_SIZE) {
        uint64_t pa = TranslateAddress(va);
        if (pa != 0) {
            mapped++;
            if (mapped < 100) {  // Show first 100 mappings
                std::cout << "  VA 0x" << std::hex << va
                         << " -> PA 0x" << pa << std::dec << "\n";
            }
        } else {
            unmapped++;
        }
    }

    std::cout << "Total: " << mapped << " mapped, " << unmapped << " unmapped pages\n";
}

}