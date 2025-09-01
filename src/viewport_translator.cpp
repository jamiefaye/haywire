#include "viewport_translator.h"
#include "qemu_connection.h"
#include <iostream>
#include <algorithm>
#include <chrono>

namespace Haywire {

ViewportTranslator::ViewportTranslator(std::shared_ptr<GuestAgent> agent) 
    : guestAgent(agent), currentPid(-1), viewportCenter(0), viewportSize(0),
      pagemapCache(std::make_unique<PagemapCache>()) {
    stats = {0, 0, 0, 0.0f};
}

ViewportTranslator::~ViewportTranslator() {
}

void ViewportTranslator::SetViewport(int pid, uint64_t centerVA, size_t viewSize) {
    // Check if we switched processes
    if (pid != currentPid && pid > 0) {
        // Clear cache when switching processes
        cache.clear();
        stats.totalEntries = 0;
    }
    
    currentPid = pid;
    viewportCenter = centerVA;
    viewportSize = viewSize;
    
    // Don't prefetch - just cache on demand
}

uint64_t ViewportTranslator::TranslateAddress(int pid, uint64_t virtualAddr) {
    PagemapEntry entry;
    if (GetTranslation(pid, virtualAddr, entry)) {
        return entry.physAddr;
    }
    return 0;
}

bool ViewportTranslator::GetTranslation(int pid, uint64_t virtualAddr, PagemapEntry& entry) {
    // Try fast pagemap cache first
    if (pagemapCache->IsValid(pid)) {
        if (pagemapCache->Lookup(virtualAddr, entry)) {
            stats.hitCount++;
            stats.hitRate = (float)stats.hitCount / (stats.hitCount + stats.missCount);
            return true;
        }
    }
    
    uint64_t pageAddr = AlignToPage(virtualAddr);
    
    // Check old cache as fallback
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
    
    // Cache miss
    stats.missCount++;
    stats.hitRate = (float)stats.hitCount / (stats.hitCount + stats.missCount);
    
    // Try QMP translation for kernel addresses (fast path - sub-ms)
    // Kernel addresses are mapped the same for all processes
    bool isKernelAddr = (virtualAddr >= 0xffff000000000000ULL);
    
    if (qemuConnection && isKernelAddr) {
        uint64_t physAddr;
        auto start = std::chrono::high_resolution_clock::now();
        
        // Use CPU 0 - kernel mappings are the same on all CPUs
        if (qemuConnection->TranslateVA2PA(0, virtualAddr, physAddr)) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            // Fill in the entry
            entry.present = true;
            entry.pfn = physAddr / PAGE_SIZE;
            entry.physAddr = physAddr;
            
            // Cache it
            cache[pid][pageAddr] = entry;
            stats.totalEntries++;
            
            // Log timing occasionally
            static int qmpCounter = 0;
            if (++qmpCounter % 100 == 0) {
                std::cerr << "QMP VA->PA translation (kernel) took " << duration << " µs" << std::endl;
            }
            
            return true;
        }
    }
    
    // For user-space addresses, we need to use guest agent to ensure
    // we get the correct process's mappings
    //
    // TODO: Future optimization:
    // 1. Use QMP to translate kernel addresses and find init_task
    // 2. Walk kernel process list to extract TTBR values for each PID
    // 3. Extend QMP command to accept TTBR for accurate user-space translation
    // This would give us fast VA->PA for all addresses, not just kernel
    
    // Fallback to guest agent (slow path - ~300ms)
    // Log cache misses periodically
    static int missCounter = 0;
    if (++missCounter % 100 == 0) {
        std::cerr << "Cache stats: " << stats.hitCount << " hits, " << stats.missCount 
                  << " misses (" << (stats.hitRate * 100) << "% hit rate)" << std::endl;
    }
    
    if (!guestAgent || !guestAgent->IsConnected()) {
        return false;
    }
    
    // On cache miss, prefetch a screen-sized batch of pages
    // This amortizes the ~300ms cost over many pages
    const size_t SCREEN_PAGES = 256;  // About 1MB worth of pages
    uint64_t batchStart = pageAddr;
    
    // Align batch to reduce fragmentation
    if (batchStart > SCREEN_PAGES * PAGE_SIZE / 2) {
        batchStart = batchStart - (SCREEN_PAGES * PAGE_SIZE / 2);
    }
    batchStart = AlignToPage(batchStart);
    
    size_t batchSize = SCREEN_PAGES * PAGE_SIZE;
    
    std::cerr << "Cache miss at VA 0x" << std::hex << virtualAddr 
              << ", prefetching " << std::dec << SCREEN_PAGES << " pages..." << std::endl;
    
    // Batch fetch translations
    std::vector<PagemapEntry> entries;
    if (guestAgent->TranslateRange(pid, batchStart, batchSize, entries)) {
        // Store all entries in cache
        uint64_t currentVA = batchStart;
        for (const auto& batchEntry : entries) {
            uint64_t currentPage = AlignToPage(currentVA);
            cache[pid][currentPage] = batchEntry;
            currentVA += PAGE_SIZE;
            stats.totalEntries++;
        }
        
        std::cerr << "Prefetched " << entries.size() << " pages into cache" << std::endl;
        
        // Find our original requested entry
        auto it = cache[pid].find(pageAddr);
        if (it != cache[pid].end()) {
            entry = it->second;
            // Recalculate physical address with correct offset
            if (entry.present) {
                uint64_t pageOffset = virtualAddr & PAGE_MASK;
                entry.physAddr = (entry.pfn * PAGE_SIZE) + pageOffset;
            }
            return true;
        }
    }
    
    // Fallback to single page if batch failed
    if (guestAgent->TranslateAddress(pid, virtualAddr, entry)) {
        // Store in cache
        cache[pid][pageAddr] = entry;
        stats.totalEntries++;
        return true;
    }
    
    return false;
}

void ViewportTranslator::PrefetchViewport() {
    // Disabled - pagemap cache provides instant lookups
    return;
    
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
    
    // Measure timing
    auto start = std::chrono::high_resolution_clock::now();
    
    // Fetch translations in bulk
    std::vector<PagemapEntry> entries;
    bool success = guestAgent->TranslateRange(currentPid, prefetchStart, prefetchSize, entries);
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    if (success) {
        // Store in cache
        uint64_t currentVA = prefetchStart;
        for (const auto& entry : entries) {
            cache[currentPid][currentVA] = entry;
            currentVA += PAGE_SIZE;
            stats.totalEntries++;
        }
        
        size_t numPages = prefetchSize / PAGE_SIZE;
        // Debug output disabled
        // std::cerr << "Prefetch: " << entries.size() << " pages in " << duration.count() 
        //           << "ms (" << (duration.count() > 0 ? entries.size() * 1000 / duration.count() : 0) 
        //           << " pages/sec)" << std::endl;
    } else {
        std::cerr << "Prefetch failed after " << duration.count() << "ms" << std::endl;
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