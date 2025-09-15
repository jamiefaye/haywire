#pragma once

#include "platform/page_walker.h"

namespace Haywire {

// x86-64-specific page table walker
class X86_64PageWalker : public PageWalker {
public:
    X86_64PageWalker(MemoryBackend* backend);
    ~X86_64PageWalker() override;

    // Set CR3 (base0), base1 is ignored for x86-64
    void SetPageTableBase(uint64_t cr3, uint64_t unused = 0) override;

    // Walk page tables to translate VA to PA
    uint64_t TranslateAddress(uint64_t virtualAddr) override;

    // Bulk translate a range
    size_t TranslateRange(uint64_t startVA, size_t numPages,
                         std::vector<uint64_t>& physAddrs) override;

    // Get page size (4KB for standard pages)
    uint64_t GetPageSize() const override { return PAGE_SIZE; }

    // Get architecture name
    const char* GetArchitectureName() const override { return "x86-64"; }

    // x86-64-specific: Check if PAE is enabled
    bool IsPAEEnabled() const { return true; }  // Always true for x86-64

    // x86-64-specific: Check if 5-level paging is enabled
    bool Is5LevelPagingEnabled() const { return use5LevelPaging; }

private:
    uint64_t cr3;
    bool use5LevelPaging;

    // x86-64 page table constants (4KB pages, 48-bit or 57-bit VA)
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
    static constexpr int TABLE_SHIFT = 9;
    static constexpr int TABLE_SIZE = 1 << TABLE_SHIFT;  // 512 entries
    static constexpr uint64_t TABLE_MASK = TABLE_SIZE - 1;

    // Level shifts for 4KB pages (4-level paging)
    static constexpr int PML4_SHIFT = 39;  // Bits 47:39
    static constexpr int PDPT_SHIFT = 30;  // Bits 38:30
    static constexpr int PD_SHIFT = 21;    // Bits 29:21
    static constexpr int PT_SHIFT = 12;    // Bits 20:12

    // Level shifts for 5-level paging
    static constexpr int PML5_SHIFT = 48;  // Bits 56:48

    // Page table entry bits
    static constexpr uint64_t PTE_PRESENT = 1ULL << 0;
    static constexpr uint64_t PTE_WRITE = 1ULL << 1;
    static constexpr uint64_t PTE_USER = 1ULL << 2;
    static constexpr uint64_t PTE_ACCESSED = 1ULL << 5;
    static constexpr uint64_t PTE_DIRTY = 1ULL << 6;
    static constexpr uint64_t PTE_PSE = 1ULL << 7;  // Page size (2MB/1GB)
    static constexpr uint64_t PTE_NX = 1ULL << 63;  // No execute

    // Address masks
    static constexpr uint64_t PTE_ADDR_MASK = 0x000FFFFFFFFFF000ULL;

    // Walk the 4-level or 5-level page table
    uint64_t WalkPageTable4Level(uint64_t va);
    uint64_t WalkPageTable5Level(uint64_t va);
};

}