#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include "guest_agent.h"

namespace Haywire {

// Viewport-aware virtual to physical address translator
// Follows the current memory view and prefetches nearby pages
class ViewportTranslator {
public:
    ViewportTranslator(std::shared_ptr<GuestAgent> agent);
    ~ViewportTranslator();
    
    // Set current viewport (what user is looking at)
    void SetViewport(int pid, uint64_t centerVA, size_t viewSize);
    
    // Translate single address (uses cache if available)
    uint64_t TranslateAddress(int pid, uint64_t virtualAddr);
    
    // Get translation with details
    bool GetTranslation(int pid, uint64_t virtualAddr, PagemapEntry& entry);
    
    // Prefetch translations for viewport area
    void PrefetchViewport();
    
    // Clear cache for specific process or all
    void ClearCache(int pid = -1);
    
    // Get cache statistics
    struct CacheStats {
        size_t totalEntries;
        size_t hitCount;
        size_t missCount;
        float hitRate;
    };
    CacheStats GetStats() const { return stats; }
    
private:
    std::shared_ptr<GuestAgent> guestAgent;
    
    // Current viewport
    int currentPid;
    uint64_t viewportCenter;
    size_t viewportSize;
    
    // Translation cache: [pid][va_page] -> PagemapEntry
    std::unordered_map<int, std::unordered_map<uint64_t, PagemapEntry>> cache;
    
    // Statistics
    mutable CacheStats stats;
    
    // Constants
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
    static constexpr size_t PREFETCH_RADIUS = 16;  // Pages around viewport
    
    // Helper to get page-aligned address
    uint64_t AlignToPage(uint64_t addr) const {
        return addr & ~PAGE_MASK;
    }
};

}