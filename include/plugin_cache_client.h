#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>

namespace Haywire {

// Client for reading VA->PA translations from QEMU plugin's shared memory
class PluginCacheClient {
public:
    PluginCacheClient();
    ~PluginCacheClient();
    
    // Connect to shared memory
    bool Connect();
    void Disconnect();
    bool IsConnected() const { return cache != nullptr; }
    
    // Lookup a single translation
    bool Lookup(uint32_t pid, uint64_t va, uint64_t& pa);
    
    // Bulk lookup for a range
    size_t LookupRange(uint32_t pid, uint64_t startVA, size_t numPages,
                      std::vector<uint64_t>& physAddrs);
    
    // Refresh local cache from shared memory
    void RefreshCache();
    
    // Get statistics
    struct Stats {
        uint64_t totalEntries;
        uint64_t localCacheHits;
        uint64_t localCacheMisses;
        uint64_t sharedLookups;
    };
    Stats GetStats() const { return stats; }
    
private:
    struct SharedCache;
    SharedCache* cache;
    int shmFd;
    
    // Local cache for faster lookups
    struct CacheKey {
        uint32_t pid;
        uint64_t va;
        
        bool operator==(const CacheKey& other) const {
            return pid == other.pid && va == other.va;
        }
    };
    
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<uint64_t>()(k.va) ^ (std::hash<uint32_t>()(k.pid) << 1);
        }
    };
    
    std::unordered_map<CacheKey, uint64_t, CacheKeyHash> localCache;
    Stats stats;
    
    static constexpr size_t MAX_LOCAL_CACHE = 100000;
};

}