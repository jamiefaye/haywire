#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <memory>
#include <atomic>
#include <iostream>
#include <algorithm>

namespace Haywire {

// Forward declarations
class KernelDiscoveryBackend;
struct ProcessInfo;
struct SectionEntry;

/**
 * PageMetadata - Tracks everything we know about a single 4KB page
 */
struct PageMetadata {
    uint64_t physicalAddr;      // Physical address (0x40000000 + offset)
    uint64_t virtualAddr;       // Virtual address (0 if unattributed)
    std::vector<uint32_t> pids; // Owning processes (empty if unattributed, multiple for shared pages)

    enum OwnershipType {
        UNATTRIBUTED = 0,       // Not claimed by any process VMA
        ANONYMOUS,              // Heap, anonymous mmap
        FILE_BACKED,            // Mapped file (not code/lib)
        SHARED_LIB,             // Shared library (.so)
        EXECUTABLE,             // Executable code
        STACK,                  // Thread stack
        HEAP,                   // Heap pages
        VDSO,                   // Virtual DSO
        VVAR,                   // Virtual var
        PAGE_CACHE,             // Kernel page cache (future)
        KERNEL_DATA,            // Kernel data structures (future)
    };
    OwnershipType ownershipType;

    uint32_t flags;             // VM_READ, VM_WRITE, VM_EXEC, VM_SHARED

    std::string filename;       // File path if file-backed (empty otherwise)

    // Constructor
    PageMetadata()
        : physicalAddr(0), virtualAddr(0),
          ownershipType(UNATTRIBUTED), flags(0) {}

    // Helper methods
    bool isAttributed() const { return !pids.empty(); }
    bool hasProcess(uint32_t pid) const {
        return std::find(pids.begin(), pids.end(), pid) != pids.end();
    }
    void addProcess(uint32_t pid) {
        if (!hasProcess(pid)) {
            pids.push_back(pid);
        }
    }
    bool isReadable() const { return flags & 0x01; }
    bool isWritable() const { return flags & 0x02; }
    bool isExecutable() const { return flags & 0x04; }
    bool isShared() const { return flags & 0x08; }

    // Get human-readable type name
    const char* getTypeName() const {
        switch (ownershipType) {
            case UNATTRIBUTED: return "Unattributed";
            case ANONYMOUS: return "Anonymous";
            case FILE_BACKED: return "File-backed";
            case SHARED_LIB: return "Shared Library";
            case EXECUTABLE: return "Executable";
            case STACK: return "Stack";
            case HEAP: return "Heap";
            case VDSO: return "vDSO";
            case VVAR: return "vvar";
            case PAGE_CACHE: return "Page Cache";
            case KERNEL_DATA: return "Kernel Data";
            default: return "???";
        }
    }
};

/**
 * PageDatabase - System-wide page tracking
 *
 * Maintains metadata for every 4KB page in VM RAM by scanning all processes.
 * Enables filtering, sorting, and accounting for memory attribution.
 */
class PageDatabase {
    // Allow BackgroundScanner to access private members
    friend class BackgroundScanner;

public:
    PageDatabase();
    ~PageDatabase();

    // Initialize with memory bounds
    void Initialize(uint64_t ramBase, uint64_t ramSize);

    // Scan all processes to populate page metadata
    // numThreads: number of worker threads (default: 4, use 0 for auto-detect)
    // Returns number of pages attributed
    size_t ScanAllProcesses(KernelDiscoveryBackend* backend, int numThreads = 4);

    // Get metadata for a physical address (returns nullptr if invalid)
    const PageMetadata* GetPageByPhysical(uint64_t physAddr) const;

    // Get metadata for a virtual address in a specific process
    const PageMetadata* GetPageByVirtual(uint32_t pid, uint64_t virtAddr) const;

    // Get all pages (for filtering/sorting) - returns a copy for thread safety
    std::vector<PageMetadata> GetAllPages() const {
        std::lock_guard<std::mutex> lock(mutex);

        // Debug: Count PIDs
        size_t pid3101Count = 0;
        for (const auto& page : pages) {
            if (page.hasProcess(3101)) pid3101Count++;
        }
        if (pid3101Count > 0) {
            std::cout << "[PageDB::GetAllPages] Returning " << pages.size() << " total pages, "
                      << pid3101Count << " have PID 3101\n";
        }

        return pages;
    }

    // Statistics
    struct Stats {
        size_t totalPages;
        size_t attributedPages;
        size_t unattributedPages;
        std::unordered_map<PageMetadata::OwnershipType, size_t> pagesByType;
        std::unordered_map<uint32_t, size_t> pagesByPID;
    };
    Stats GetStats() const;

    // Clear and reset database
    void Clear();

    // Background scanning with priority queue
    // rescanIntervalSec: how often to do a full rescan (default 30 seconds, 0 = no rescan)
    void StartBackgroundScanning(KernelDiscoveryBackend* backend, int rescanIntervalSec = 30);
    void StopBackgroundScanning();

    // Scan a single PID immediately (blocks until complete)
    // Returns number of pages attributed
    size_t ScanSinglePID(uint32_t pid, KernelDiscoveryBackend* backend);

    // Request priority scan for a PID (non-blocking, queues for background thread)
    void RequestPriorityScan(uint32_t pid);

    // Get all pages for a specific PID (returns empty if PID not scanned yet)
    std::vector<const PageMetadata*> GetPagesForPID(uint32_t pid) const;

    // Get data for a PID in format compatible with visualizer
    // Returns sections and PTEs (sections are coalesced from pages)
    bool GetPIDData(uint32_t pid,
                    std::vector<SectionEntry>& sections,
                    std::unordered_map<uint64_t, uint64_t>& ptes) const;

    // Check if a PID has been scanned
    bool IsPIDScanned(uint32_t pid) const;

    // Get process name for a PID (returns empty string if not found)
    std::string GetProcessName(uint32_t pid) const;

    // Get all known PIDs and their names
    std::unordered_map<uint32_t, std::string> GetAllProcessNames() const;

    // Query scan status
    bool IsScanning() const;
    bool IsFullScanComplete() const;
    size_t GetScannedProcessCount() const;
    size_t GetTotalProcessCount() const;
    size_t GetScanGeneration() const;  // Increments each time a full scan completes

private:
    // Page storage: indexed by (physAddr - ramBase) / 4096
    std::vector<PageMetadata> pages;

    // Reverse index: (pid, virtualAddr/4096) -> page index
    // Allows fast lookup of "what page is at VA X in process Y?"
    std::unordered_map<uint64_t, size_t> virtualLookup;  // key = (pid << 32) | (va >> 12)

    uint64_t ramBase;
    uint64_t ramSize;
    size_t numPages;

    // Thread safety
    mutable std::mutex mutex;

    // Background scanning
    std::unique_ptr<class BackgroundScanner> scanner;

    // Scan state tracking
    std::atomic<bool> fullScanComplete{false};
    std::atomic<size_t> scannedProcessCount{0};
    std::atomic<size_t> totalProcessCount{0};
    std::atomic<size_t> scanGeneration{0};  // Increments each time a full scan completes
    std::unordered_set<uint32_t> scannedPIDs;  // PIDs we've scanned
    std::unordered_map<uint32_t, std::string> processNames;  // PID → process name

    // Helper methods
    size_t ScanSingleProcess(const ProcessInfo& proc, KernelDiscoveryBackend* backend);
    size_t PhysToIndex(uint64_t physAddr) const {
        return (physAddr - ramBase) / 4096;
    }

    uint64_t IndexToPhys(size_t index) const {
        return ramBase + (index * 4096);
    }

    uint64_t MakeVirtualKey(uint32_t pid, uint64_t virtualAddr) const {
        return (static_cast<uint64_t>(pid) << 32) | (virtualAddr >> 12);
    }

    // Multi-threaded scanning helpers
    size_t ScanProcessRange(KernelDiscoveryBackend* backend,
                           const std::vector<ProcessInfo>& processes,
                           size_t start, size_t end);

    size_t AttributeProcessPages(const ProcessInfo& proc,
                                const std::vector<SectionEntry>& sections,
                                const std::unordered_map<uint64_t, uint64_t>& ptes);

    PageMetadata::OwnershipType ConvertOwnershipType(uint32_t sectionType);
    uint32_t ConvertFlags(uint32_t sectionPerms);
};

} // namespace Haywire
