#include "pte_walker.h"
#include <iostream>
#include <chrono>

namespace Haywire {

PTEWalker::PTEWalker() {
}

PTEWalker::~PTEWalker() {
    Stop();
}

void PTEWalker::Start(PALookupCache* cache,
                     AddressSpaceFlattener* flattener,
                     KernelViewportTranslator* translator,
                     int pid,
                     int patrolHz) {
    if (running_.load()) {
        std::cerr << "PTEWalker already running\n";
        return;
    }

    if (!cache || !flattener || !translator || pid <= 0) {
        std::cerr << "PTEWalker: Invalid parameters\n";
        return;
    }

    cache_ = cache;
    flattener_ = flattener;
    translator_ = translator;
    pid_ = pid;
    patrolHz_ = patrolHz;

    // Enable double-buffering on cache
    cache_->EnableDoubleBuffering();

    // Reset stats
    scanCount_ = 0;
    newMappings_ = 0;
    unmappings_ = 0;
    lastScanDurationMs_ = 0;

    // Start worker thread
    running_ = true;
    workerThread_ = std::thread(&PTEWalker::PatrolLoop, this);

    std::cout << "PTEWalker: Started patrol at " << patrolHz << " Hz for PID " << pid << "\n";
}

void PTEWalker::Stop() {
    if (!running_.load()) {
        return;
    }

    running_ = false;

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    std::cout << "PTEWalker: Stopped patrol\n";
}

PTEWalker::Stats PTEWalker::GetStats() const {
    Stats stats;
    stats.scanCount = scanCount_.load();
    stats.newMappings = newMappings_.load();
    stats.unmappings = unmappings_.load();
    stats.lastScanDurationMs = lastScanDurationMs_.load();
    return stats;
}

void PTEWalker::PatrolLoop() {
    auto intervalMs = 1000 / patrolHz_;

    while (running_.load()) {
        auto startTime = std::chrono::steady_clock::now();

        // Scan all PTEs and update cache
        ScanAllPTEs();

        auto endTime = std::chrono::steady_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        lastScanDurationMs_ = durationMs;

        scanCount_++;

        // Sleep until next patrol cycle
        auto sleepMs = intervalMs - durationMs;
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
}

void PTEWalker::ScanAllPTEs() {
    if (!cache_ || !flattener_ || !translator_) return;

    // Begin update cycle (write to inactive buffer)
    cache_->BeginUpdate();

    int newMappingsThisScan = 0;
    int unmappingsThisScan = 0;

    size_t numPages = cache_->Size();
    const size_t PAGE_SIZE = 4096;

    // Walk all pages in flat address space
    for (size_t pageIdx = 0; pageIdx < numPages; pageIdx++) {
        // Check stop flag every 1000 pages to allow quick shutdown
        if (pageIdx % 1000 == 0 && !running_.load()) {
            std::cout << "PTEWalker: Scan interrupted at page " << pageIdx << "/" << numPages << "\n";
            return;  // Exit early without committing partial update
        }

        uint64_t flatAddr = pageIdx * PAGE_SIZE;

        // Find which region this belongs to (skip gaps)
        const auto* region = flattener_->GetRegionForFlat(flatAddr);
        if (!region) continue;

        // Calculate VA from flat address
        uint64_t offsetInRegion = flatAddr - region->flatStart;
        uint64_t va = region->virtualStart + offsetInRegion;
        uint64_t pageVA = (va / PAGE_SIZE) * PAGE_SIZE;

        // Translate VA to PA
        uint64_t pagePA = translator_->TranslateAddress(pid_, pageVA);

        // Get old value from cache
        uint64_t oldPA = cache_->GetRaw(pageIdx);

        // Detect changes
        if (pagePA == 0) {
            // Page is unmapped
            if (oldPA != PALookupCache::UNMAPPED_SENTINEL && oldPA != PALookupCache::NOT_TRANSLATED) {
                // Was mapped, now unmapped
                unmappingsThisScan++;
            }
            cache_->SetUnmapped(pageIdx);
        } else {
            // Page is mapped
            if (oldPA == PALookupCache::UNMAPPED_SENTINEL || oldPA == PALookupCache::NOT_TRANSLATED) {
                // Was unmapped/unknown, now mapped
                newMappingsThisScan++;
            } else if (oldPA != pagePA) {
                // PA changed (remapped)
                newMappingsThisScan++;
            }
            cache_->SetPA(pageIdx, pagePA);
        }
    }

    // Commit update (atomic swap)
    cache_->CommitUpdate();

    // Update cumulative stats
    if (newMappingsThisScan > 0 || unmappingsThisScan > 0) {
        newMappings_ += newMappingsThisScan;
        unmappings_ += unmappingsThisScan;
        std::cout << "PTEWalker: Detected changes - +" << newMappingsThisScan
                  << " new, -" << unmappingsThisScan << " unmapped\n";
    }
}

} // namespace Haywire
