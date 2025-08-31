#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

namespace Haywire {

class VATranslator {
public:
    VATranslator();
    ~VATranslator();
    
    // Single address translation (uses cache if available)
    uint64_t TranslateVirtualToPhysical(int pid, uint64_t va);
    
    // Bulk translation for a memory range
    bool TranslateRange(int pid, uint64_t va_start, size_t length,
                       std::vector<uint64_t>& physical_addresses);
    
    // Prefetch translations for a range (background)
    void PrefetchTranslations(int pid, uint64_t va_start, size_t length);
    
    // Clear cache for a specific process
    void ClearCache(int pid);
    
    // Get translation via QMP/monitor (slower but no guest agent)
    uint64_t TranslateViaMonitor(int pid, uint64_t va);
    
private:
    // Cache: [pid][va_page] -> pa_page
    std::unordered_map<int, std::unordered_map<uint64_t, uint64_t>> cache;
    
    // Read /proc/[pid]/pagemap via memory dump
    // Instead of guest agent, we could dump it to shared memory
    bool ReadPagemapDirect(int pid, uint64_t va_start, size_t page_count,
                          std::vector<uint64_t>& pfns);
    
    // Parse pagemap entry
    struct PagemapEntry {
        uint64_t pfn : 55;
        uint64_t soft_dirty : 1;
        uint64_t reserved : 4;
        uint64_t file_shared : 1;
        uint64_t swapped : 1;
        uint64_t present : 1;
    };
    
    PagemapEntry ParsePagemapEntry(uint64_t entry);
    
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
};

}