#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace Haywire {

// Cache for VA→PA translations with lock-free double-buffering
// Readers use activeTable (lock-free atomic load)
// Writer updates inactive table, then atomically swaps pointer
class PALookupCache {
public:
    static constexpr uint64_t UNMAPPED_SENTINEL = 0xFFFFFFFFFFFFFFFFULL;
    static constexpr uint64_t NOT_TRANSLATED = 0;

    PALookupCache() : tableA(), tableB(), activeTable(&tableA), writingTable(nullptr), doubleBuffered(false) {}

    // Enable double-buffering for lock-free updates
    void EnableDoubleBuffering() {
        if (doubleBuffered) return;

        // Copy current state to both tables
        tableB = tableA;
        doubleBuffered = true;
    }

    // Initialize cache with given number of pages
    void Resize(size_t numPages) {
        auto* active = GetActiveTable();
        active->resize(numPages, NOT_TRANSLATED);

        if (doubleBuffered) {
            auto* inactive = GetInactiveTable();
            inactive->resize(numPages, NOT_TRANSLATED);
        }
    }

    // Get physical address for page index (lock-free read from active table)
    uint64_t GetPA(size_t pageIdx) const {
        auto* active = GetActiveTable();
        if (pageIdx >= active->size()) return 0;
        uint64_t pa = (*active)[pageIdx];
        return (pa == UNMAPPED_SENTINEL || pa == NOT_TRANSLATED) ? 0 : pa;
    }

    // Get raw value (including sentinels) for page index
    uint64_t GetRaw(size_t pageIdx) const {
        auto* active = GetActiveTable();
        if (pageIdx >= active->size()) return 0;
        return (*active)[pageIdx];
    }

    // Set physical address for page index (writes to active table, or writing table if in update)
    void SetPA(size_t pageIdx, uint64_t pa) {
        auto* target = writingTable ? writingTable : GetActiveTable();
        if (pageIdx < target->size()) {
            (*target)[pageIdx] = pa;
        }
    }

    // Mark page as unmapped
    void SetUnmapped(size_t pageIdx) {
        auto* target = writingTable ? writingTable : GetActiveTable();
        if (pageIdx < target->size()) {
            (*target)[pageIdx] = UNMAPPED_SENTINEL;
        }
    }

    // Check if page is known to be unmapped (without triggering translation)
    bool IsUnmapped(size_t pageIdx) const {
        auto* active = GetActiveTable();
        if (pageIdx >= active->size()) return false;
        return (*active)[pageIdx] == UNMAPPED_SENTINEL;
    }

    // Check if page has been translated yet
    bool IsTranslated(size_t pageIdx) const {
        auto* active = GetActiveTable();
        if (pageIdx >= active->size()) return false;
        return (*active)[pageIdx] != NOT_TRANSLATED;
    }

    // Get number of pages in cache
    size_t Size() const {
        auto* active = GetActiveTable();
        return active->size();
    }

    // Clear all entries and reset to single-buffered mode
    void Clear() {
        tableA.clear();
        tableB.clear();
        activeTable = &tableA;
        writingTable = nullptr;
        doubleBuffered = false;
    }

    // Writer API: Begin update cycle (for PTE walker thread)
    void BeginUpdate() {
        if (!doubleBuffered) return;

        // Select inactive table for writing
        auto* active = activeTable.load(std::memory_order_acquire);
        writingTable = (active == &tableA) ? &tableB : &tableA;

        // Copy current state to writing table
        *writingTable = *active;
    }

    // Writer API: Commit update (atomic swap)
    void CommitUpdate() {
        if (!doubleBuffered || !writingTable) return;

        // Atomically swap to new table
        activeTable.store(writingTable, std::memory_order_release);
        writingTable = nullptr;
    }

private:
    // Double-buffered tables
    std::vector<uint64_t> tableA;
    std::vector<uint64_t> tableB;

    // Active table pointer (readers use this)
    mutable std::atomic<std::vector<uint64_t>*> activeTable;

    // Writing table pointer (writer uses this during update)
    std::vector<uint64_t>* writingTable = nullptr;

    // Double-buffering enabled flag
    bool doubleBuffered;

    // Helper to get active table (with const correctness)
    std::vector<uint64_t>* GetActiveTable() const {
        return activeTable.load(std::memory_order_acquire);
    }

    std::vector<uint64_t>* GetInactiveTable() {
        auto* active = activeTable.load(std::memory_order_acquire);
        return (active == &tableA) ? &tableB : &tableA;
    }
};

} // namespace Haywire
