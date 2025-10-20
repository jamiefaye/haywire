#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>
#include <memory>

namespace Haywire {

// Forward declarations
class KernelDiscoveryBackend;

/**
 * PageMetadata - Tracks everything we know about a single 4KB page
 */
struct PageMetadata {
    uint64_t physicalAddr;      // Physical address (0x40000000 + offset)
    uint64_t virtualAddr;       // Virtual address (0 if unattributed)
    uint32_t pid;               // Owning process (0 if unattributed)

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
        : physicalAddr(0), virtualAddr(0), pid(0),
          ownershipType(UNATTRIBUTED), flags(0) {}

    // Helper methods
    bool isAttributed() const { return pid != 0; }
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
public:
    PageDatabase();
    ~PageDatabase();

    // Initialize with memory bounds
    void Initialize(uint64_t ramBase, uint64_t ramSize);

    // Scan all processes to populate page metadata
    // Returns number of pages attributed
    size_t ScanAllProcesses(KernelDiscoveryBackend* backend);

    // Get metadata for a physical address (returns nullptr if invalid)
    const PageMetadata* GetPageByPhysical(uint64_t physAddr) const;

    // Get metadata for a virtual address in a specific process
    const PageMetadata* GetPageByVirtual(uint32_t pid, uint64_t virtAddr) const;

    // Get all pages (for filtering/sorting)
    const std::vector<PageMetadata>& GetAllPages() const { return pages; }

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

    // Background scanning (future: periodic refresh)
    void StartBackgroundScanning(KernelDiscoveryBackend* backend, int intervalMs = 5000);
    void StopBackgroundScanning();

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

    // Helper methods
    size_t PhysToIndex(uint64_t physAddr) const {
        return (physAddr - ramBase) / 4096;
    }

    uint64_t IndexToPhys(size_t index) const {
        return ramBase + (index * 4096);
    }

    uint64_t MakeVirtualKey(uint32_t pid, uint64_t virtualAddr) const {
        return (static_cast<uint64_t>(pid) << 32) | (virtualAddr >> 12);
    }
};

} // namespace Haywire
