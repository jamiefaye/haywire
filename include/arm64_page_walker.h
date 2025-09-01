#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace Haywire {

class MemoryBackend;

// ARM64 page table walker using direct physical memory access
class ARM64PageWalker {
public:
    ARM64PageWalker(MemoryBackend* backend);
    ~ARM64PageWalker();
    
    // Set the page table base (TTBR) for a process
    // This needs to be obtained via guest agent or other means
    void SetPageTableBase(uint64_t ttbr0, uint64_t ttbr1);
    
    // Walk page tables to translate VA to PA
    uint64_t TranslateAddress(uint64_t virtualAddr);
    
    // Bulk translate a range
    size_t TranslateRange(uint64_t startVA, size_t numPages,
                         std::vector<uint64_t>& physAddrs);
    
    // Dump all mappings (for debugging)
    void DumpMappings(uint64_t maxVA = 0x1000000000ULL);
    
private:
    MemoryBackend* memory;
    uint64_t ttbr0;
    uint64_t ttbr1;
    
    // ARM64 page table constants (4KB pages, 48-bit VA)
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
    static constexpr int TABLE_SHIFT = 9;
    static constexpr int TABLE_SIZE = 1 << TABLE_SHIFT;  // 512 entries
    static constexpr uint64_t TABLE_MASK = TABLE_SIZE - 1;
    
    // Level shifts for 4KB pages
    static constexpr int L0_SHIFT = 39;  // Bits 47:39
    static constexpr int L1_SHIFT = 30;  // Bits 38:30  
    static constexpr int L2_SHIFT = 21;  // Bits 29:21
    static constexpr int L3_SHIFT = 12;  // Bits 20:12
    
    // Descriptor bits
    static constexpr uint64_t DESC_VALID = 1ULL << 0;
    static constexpr uint64_t DESC_TABLE = 1ULL << 1;  // For L0-L2
    static constexpr uint64_t DESC_AF = 1ULL << 10;     // Access flag
    
    // Read 64-bit value from guest physical memory
    uint64_t ReadPhys64(uint64_t paddr);
    
    // Walk the 4-level page table
    uint64_t WalkPageTable(uint64_t va, uint64_t ttbr);
};

}