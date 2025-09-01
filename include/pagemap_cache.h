#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <chrono>
#include "guest_agent.h"

namespace Haywire {

// Cache entire pagemap file for a process
// Much faster than individual lookups through guest agent
class PagemapCache {
public:
    PagemapCache();
    ~PagemapCache();
    
    // Load entire pagemap for process (expensive but one-time)
    bool LoadProcess(GuestAgent* agent, int pid);
    
    // Fast local lookup (no network/agent overhead)
    bool Lookup(uint64_t virtualAddr, PagemapEntry& entry) const;
    
    // Get multiple entries efficiently
    bool LookupRange(uint64_t startVA, size_t numPages, 
                     std::vector<PagemapEntry>& entries) const;
    
    // Check if cache is valid for PID
    bool IsValid(int pid) const { return cachedPid == pid && !pagemapData.empty(); }
    
    // Get cache statistics
    size_t GetCacheSize() const { return pagemapData.size(); }
    int GetCachedPid() const { return cachedPid; }
    
private:
    int cachedPid;
    std::vector<uint64_t> pagemapData;  // Raw pagemap entries
    std::chrono::steady_clock::time_point loadTime;
    
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t MAX_CACHE_SIZE = 256 * 1024 * 1024; // 256MB max
};

}