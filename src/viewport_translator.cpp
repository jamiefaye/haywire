#include "viewport_translator.h"
#include <iostream>
#include <algorithm>

namespace Haywire {

ViewportTranslator::ViewportTranslator(std::shared_ptr<GuestAgent> agent) 
    : guestAgent(agent), currentPid(-1), viewportCenter(0), viewportSize(0) {
    stats = {0, 0, 0, 0.0f};
}

ViewportTranslator::~ViewportTranslator() {
}

void ViewportTranslator::SetViewport(int pid, uint64_t centerVA, size_t viewSize) {
    currentPid = pid;
    viewportCenter = centerVA;
    viewportSize = viewSize;
    
    // Automatically prefetch when viewport changes
    PrefetchViewport();
}

uint64_t ViewportTranslator::TranslateAddress(int pid, uint64_t virtualAddr) {
    PagemapEntry entry;
    if (GetTranslation(pid, virtualAddr, entry)) {
        return entry.physAddr;
    }
    return 0;
}

bool ViewportTranslator::GetTranslation(int pid, uint64_t virtualAddr, PagemapEntry& entry) {
    uint64_t pageAddr = AlignToPage(virtualAddr);
    
    // Check cache first
    auto pidIt = cache.find(pid);
    if (pidIt != cache.end()) {
        auto pageIt = pidIt->second.find(pageAddr);
        if (pageIt != pidIt->second.end()) {
            entry = pageIt->second;
            
            // Recalculate physical address with correct offset
            if (entry.present) {
                uint64_t pageOffset = virtualAddr & PAGE_MASK;
                entry.physAddr = (entry.pfn * PAGE_SIZE) + pageOffset;
            }
            
            stats.hitCount++;
            stats.hitRate = (float)stats.hitCount / (stats.hitCount + stats.missCount);
            return true;
        }
    }
    
    // Cache miss - fetch from guest agent
    stats.missCount++;
    stats.hitRate = (float)stats.hitCount / (stats.hitCount + stats.missCount);
    
    if (!guestAgent || !guestAgent->IsConnected()) {
        return false;
    }
    
    // Fetch single page translation
    if (guestAgent->TranslateAddress(pid, virtualAddr, entry)) {
        // Store in cache
        cache[pid][pageAddr] = entry;
        stats.totalEntries++;
        return true;
    }
    
    return false;
}

void ViewportTranslator::PrefetchViewport() {
    if (!guestAgent || !guestAgent->IsConnected() || currentPid < 0) {
        return;
    }
    
    // Calculate range to prefetch
    uint64_t prefetchStart = viewportCenter;
    size_t prefetchSize = viewportSize;
    
    // Expand by prefetch radius
    if (prefetchStart > PREFETCH_RADIUS * PAGE_SIZE) {
        prefetchStart -= PREFETCH_RADIUS * PAGE_SIZE;
        prefetchSize += 2 * PREFETCH_RADIUS * PAGE_SIZE;
    } else {
        prefetchSize += PREFETCH_RADIUS * PAGE_SIZE;
        prefetchStart = 0;
    }
    
    // Align to page boundaries
    prefetchStart = AlignToPage(prefetchStart);
    
    std::cerr << "Prefetching VA range 0x" << std::hex << prefetchStart 
              << " - 0x" << (prefetchStart + prefetchSize) 
              << " for PID " << std::dec << currentPid << std::endl;
    
    // Fetch translations in bulk
    std::vector<PagemapEntry> entries;
    if (guestAgent->TranslateRange(currentPid, prefetchStart, prefetchSize, entries)) {
        // Store in cache
        uint64_t currentVA = prefetchStart;
        for (const auto& entry : entries) {
            cache[currentPid][currentVA] = entry;
            currentVA += PAGE_SIZE;
            stats.totalEntries++;
        }
        
        std::cerr << "Prefetched " << entries.size() << " page translations" << std::endl;
    }
}

void ViewportTranslator::ClearCache(int pid) {
    if (pid < 0) {
        // Clear all
        cache.clear();
        stats.totalEntries = 0;
    } else {
        // Clear specific PID
        auto it = cache.find(pid);
        if (it != cache.end()) {
            stats.totalEntries -= it->second.size();
            cache.erase(it);
        }
    }
}

}