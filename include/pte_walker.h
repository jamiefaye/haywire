#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include "pa_lookup_cache.h"
#include "address_space_flattener.h"
#include "kernel_viewport_translator.h"

namespace Haywire {

// Background thread that patrols page table entries (PTEs) to detect mapping changes
// Updates PA lookup cache at configurable rate (default 1 Hz)
class PTEWalker {
public:
    PTEWalker();
    ~PTEWalker();

    // Configure and start patrol
    void Start(PALookupCache* cache,
              AddressSpaceFlattener* flattener,
              KernelViewportTranslator* translator,
              int pid,
              int patrolHz = 1);

    // Stop patrol thread
    void Stop();

    // Check if patrol is running
    bool IsRunning() const { return running_.load(); }

    // Get statistics
    struct Stats {
        uint64_t scanCount = 0;        // Total scans completed
        uint64_t newMappings = 0;      // Cumulative new mappings detected
        uint64_t unmappings = 0;       // Cumulative unmappings detected
        uint64_t lastScanDurationMs = 0;  // Duration of last scan
    };
    Stats GetStats() const;

private:
    // Worker thread function
    void PatrolLoop();

    // Scan all PTEs and update cache
    void ScanAllPTEs();

    // Configuration
    PALookupCache* cache_ = nullptr;
    AddressSpaceFlattener* flattener_ = nullptr;
    KernelViewportTranslator* translator_ = nullptr;
    int pid_ = 0;
    int patrolHz_ = 1;

    // Thread state
    std::thread workerThread_;
    std::atomic<bool> running_{false};

    // Statistics (lock-free reads, only writer updates)
    std::atomic<uint64_t> scanCount_{0};
    std::atomic<uint64_t> newMappings_{0};
    std::atomic<uint64_t> unmappings_{0};
    std::atomic<uint64_t> lastScanDurationMs_{0};
};

} // namespace Haywire
