#include "pagemap_cache.h"
#include <iostream>
#include <sstream>
#include <cstring>

namespace Haywire {

PagemapCache::PagemapCache() : cachedPid(-1) {
}

PagemapCache::~PagemapCache() {
}

bool PagemapCache::LoadProcess(GuestAgent* agent, int pid) {
    if (!agent || !agent->IsConnected()) {
        return false;
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // ABORT - this approach is too slow for sparse address spaces
    std::cerr << "Pagemap cache disabled - sparse address space too large" << std::endl;
    cachedPid = -1;  // Mark as invalid
    return false;
    
    // OLD CODE - keeping for reference but disabled
    // Get process memory size to determine pagemap size
    // First get the last mapped address from /proc/pid/maps
    std::stringstream cmd;
    cmd << "tail -1 /proc/" << pid << "/maps | cut -d' ' -f1 | cut -d'-' -f2";
    
    std::string lastAddrStr;
    if (!agent->ExecuteCommand(cmd.str(), lastAddrStr)) {
        std::cerr << "Failed to get memory range for PID " << pid << std::endl;
        return false;
    }
    
    // Parse the hex address
    uint64_t lastAddr = 0;
    std::stringstream ss(lastAddrStr);
    ss >> std::hex >> lastAddr;
    
    if (lastAddr == 0) {
        std::cerr << "Invalid last address for PID " << pid << std::endl;
        return false;
    }
    
    // Calculate number of pages
    uint64_t numPages = (lastAddr + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t pagemapSize = numPages * 8;  // 8 bytes per entry
    
    // Limit to reasonable size
    if (pagemapSize > MAX_CACHE_SIZE) {
        std::cerr << "Pagemap too large: " << pagemapSize / (1024*1024) 
                  << "MB, limiting to " << MAX_CACHE_SIZE / (1024*1024) << "MB" << std::endl;
        numPages = MAX_CACHE_SIZE / 8;
        pagemapSize = MAX_CACHE_SIZE;
    }
    
    std::cerr << "Loading pagemap for PID " << pid << ": " 
              << numPages << " pages (" << pagemapSize / 1024 << " KB)" << std::endl;
    
    // Read entire pagemap in chunks (to avoid command line limits)
    pagemapData.clear();
    pagemapData.reserve(numPages);
    
    const size_t CHUNK_SIZE = 16384;  // 16K pages per chunk (128KB)
    for (uint64_t offset = 0; offset < numPages; offset += CHUNK_SIZE) {
        size_t pagesToRead = std::min(CHUNK_SIZE, size_t(numPages - offset));
        
        std::stringstream readCmd;
        readCmd << "dd if=/proc/" << pid << "/pagemap bs=8 skip=" << offset 
                << " count=" << pagesToRead << " 2>/dev/null | base64";
        
        std::string output;
        if (!agent->ExecuteCommand(readCmd.str(), output)) {
            std::cerr << "Failed to read pagemap chunk at offset " << offset << std::endl;
            return false;
        }
        
        // Decode base64
        std::string decoded = agent->DecodeBase64(output);
        if (decoded.size() < pagesToRead * 8) {
            std::cerr << "Incomplete pagemap data: got " << decoded.size() 
                      << " bytes, expected " << (pagesToRead * 8) << std::endl;
            // Continue anyway with what we got
        }
        
        // Parse entries
        for (size_t i = 0; i < decoded.size() / 8; i++) {
            uint64_t entry;
            memcpy(&entry, decoded.data() + i * 8, 8);
            pagemapData.push_back(entry);
        }
        
        // Progress indicator
        if (offset % (CHUNK_SIZE * 8) == 0) {
            std::cerr << "  Loaded " << pagemapData.size() << "/" << numPages 
                      << " pages (" << (offset * 100 / numPages) << "%)" << std::endl;
        }
    }
    
    cachedPid = pid;
    loadTime = std::chrono::steady_clock::now();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cerr << "Pagemap cache loaded: " << pagemapData.size() << " entries in " 
              << duration.count() << "ms (" 
              << (duration.count() > 0 ? pagemapData.size() * 1000 / duration.count() : 0) 
              << " pages/sec)" << std::endl;
    
    return true;
}

bool PagemapCache::Lookup(uint64_t virtualAddr, PagemapEntry& entry) const {
    if (pagemapData.empty()) {
        return false;
    }
    
    uint64_t pageNum = virtualAddr / PAGE_SIZE;
    if (pageNum >= pagemapData.size()) {
        return false;
    }
    
    uint64_t pagemapEntry = pagemapData[pageNum];
    
    entry.present = (pagemapEntry >> 63) & 1;
    entry.swapped = (pagemapEntry >> 62) & 1;
    entry.pfn = pagemapEntry & ((1ULL << 55) - 1);
    
    uint64_t pageOffset = virtualAddr & (PAGE_SIZE - 1);
    if (entry.present) {
        entry.physAddr = (entry.pfn * PAGE_SIZE) + pageOffset;
    } else {
        entry.physAddr = 0;
    }
    
    return true;
}

bool PagemapCache::LookupRange(uint64_t startVA, size_t numPages, 
                               std::vector<PagemapEntry>& entries) const {
    entries.clear();
    entries.reserve(numPages);
    
    for (size_t i = 0; i < numPages; i++) {
        PagemapEntry entry;
        if (Lookup(startVA + i * PAGE_SIZE, entry)) {
            entries.push_back(entry);
        } else {
            // Return partial result
            break;
        }
    }
    
    return !entries.empty();
}

}