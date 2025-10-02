#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace Haywire {

// Cache for VA→PA translations
// Currently single-buffered, will evolve to double-buffered for lock-free updates
class PALookupCache {
public:
    static constexpr uint64_t UNMAPPED_SENTINEL = 0xFFFFFFFFFFFFFFFFULL;
    static constexpr uint64_t NOT_TRANSLATED = 0;

    PALookupCache() = default;

    // Initialize cache with given number of pages
    void Resize(size_t numPages) {
        table_.resize(numPages, NOT_TRANSLATED);
    }

    // Get physical address for page index
    // Returns 0 if not translated or unmapped
    uint64_t GetPA(size_t pageIdx) const {
        if (pageIdx >= table_.size()) return 0;
        uint64_t pa = table_[pageIdx];
        return (pa == UNMAPPED_SENTINEL || pa == NOT_TRANSLATED) ? 0 : pa;
    }

    // Get raw value (including sentinels) for page index
    uint64_t GetRaw(size_t pageIdx) const {
        if (pageIdx >= table_.size()) return 0;
        return table_[pageIdx];
    }

    // Set physical address for page index
    void SetPA(size_t pageIdx, uint64_t pa) {
        if (pageIdx < table_.size()) {
            table_[pageIdx] = pa;
        }
    }

    // Mark page as unmapped
    void SetUnmapped(size_t pageIdx) {
        if (pageIdx < table_.size()) {
            table_[pageIdx] = UNMAPPED_SENTINEL;
        }
    }

    // Check if page is known to be unmapped (without triggering translation)
    bool IsUnmapped(size_t pageIdx) const {
        if (pageIdx >= table_.size()) return false;
        return table_[pageIdx] == UNMAPPED_SENTINEL;
    }

    // Check if page has been translated yet
    bool IsTranslated(size_t pageIdx) const {
        if (pageIdx >= table_.size()) return false;
        return table_[pageIdx] != NOT_TRANSLATED;
    }

    // Get number of pages in cache
    size_t Size() const { return table_.size(); }

    // Clear all entries
    void Clear() { table_.clear(); }

private:
    std::vector<uint64_t> table_;
};

} // namespace Haywire
