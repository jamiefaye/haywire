#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "address_space_flattener.h"
#include "kernel_viewport_translator.h"
// #include "beacon_translator.h"  // OBSOLETE - using kernel discovery
#include "qemu_connection.h"

namespace Haywire {

// Use ViewportTranslator as alias for KernelViewportTranslator
using ViewportTranslator = KernelViewportTranslator;

// Reads memory from flattened/crunched address space
// Transparently handles VA->PA translation and concatenates regions
class CrunchedMemoryReader {
public:
    CrunchedMemoryReader();
    ~CrunchedMemoryReader();
    
    // Set components
    void SetFlattener(AddressSpaceFlattener* flattener) { 
        this->flattener = flattener; 
    }
    void SetTranslator(std::shared_ptr<ViewportTranslator> translator) {
        this->translator = translator;
    }
    // OBSOLETE: BeaconTranslator methods
    // void SetBeaconTranslator(std::shared_ptr<BeaconTranslator> beaconTranslator) {
    //     this->beaconTranslator = beaconTranslator;
    // }
    // std::shared_ptr<BeaconTranslator> GetBeaconTranslator() const {
    //     return beaconTranslator;
    // }
    void SetConnection(QemuConnection* qemu) {
        this->qemu = qemu;
    }
    QemuConnection* GetConnection() const { return qemu; }
    void SetPID(int pid) {
        this->targetPid = pid;
        InitializeRenderCache();
    }

    // Initialize rendering PA cache (called when PID changes)
    void InitializeRenderCache();
    
    // Read from crunched address space
    // flatAddress: Position in flattened space (0 to totalMappedSize)
    // size: Number of bytes to read
    // Returns actual bytes read
    size_t ReadCrunchedMemory(uint64_t flatAddress, size_t size,
                             std::vector<uint8_t>& buffer);

    // Test if a page contains any non-zero bytes (zero-copy when possible)
    // flatAddress: Position in flattened space (0 to totalMappedSize)
    // size: Number of bytes to test (typically 4096)
    // Returns true if any non-zero bytes found
    bool TestPageNonZero(uint64_t flatAddress, size_t size = 4096);

    // Get direct pointer to memory at flat address (zero-copy, uses PA lookup table)
    // Returns nullptr if address not mapped or lookup table not available
    // WARNING: Pointer is only valid while memory backend remains mapped
    const uint8_t* GetDirectPointer(uint64_t flatAddress);

    // Get total size of crunched space
    uint64_t GetCrunchedSize() const {
        return flattener ? flattener->GetFlatSize() : 0;
    }

    // Get flattener for region queries
    AddressSpaceFlattener* GetFlattener() const {
        return flattener;
    }

    // Get info about current position
    struct PositionInfo {
        uint64_t flatAddr;      // Position in crunched space
        uint64_t virtualAddr;   // Corresponding VA
        uint64_t physicalAddr;  // Corresponding PA (if translated)
        std::string regionName; // Current region name
        bool isValid;           // Successfully translated
    };
    PositionInfo GetPositionInfo(uint64_t flatAddress) const;
    
private:
    AddressSpaceFlattener* flattener;
    std::shared_ptr<ViewportTranslator> translator;
    // std::shared_ptr<BeaconTranslator> beaconTranslator;  // OBSOLETE
    QemuConnection* qemu;
    int targetPid;

    // Read cache for efficiency
    struct CacheEntry {
        uint64_t flatStart;
        uint64_t flatEnd;
        std::vector<uint8_t> data;
    };
    std::vector<CacheEntry> cache;
    static constexpr size_t MAX_CACHE_ENTRIES = 10;

    // Rendering-specific VA→PA lookup table (lock-free, sized to crunched space)
    // Indexed by (flatAddress / 4096), value is PA of that page
    // Special values: 0 = not yet translated, 1 = unmapped/bad (skip immediately)
    mutable std::vector<uint64_t> renderPACache;
    static constexpr size_t PAGE_SIZE = 4096;
    static constexpr uint64_t PA_NOT_TRANSLATED = 0;
    static constexpr uint64_t PA_UNMAPPED = 1;  // Page exists but not mapped, skip it
    
    // Helper to read a single region
    bool ReadRegion(const AddressSpaceFlattener::MappedRegion& region,
                   uint64_t offsetInRegion, size_t bytesToRead,
                   std::vector<uint8_t>& buffer);
};

// Modified memory block for crunched display
struct CrunchedMemoryBlock {
    uint64_t flatAddress;        // Position in flattened space
    std::vector<uint8_t> data;    // Concatenated memory from multiple regions
    
    struct RegionMarker {
        size_t offset;           // Offset in data where region starts
        std::string name;        // Region name for display
        uint64_t virtualAddr;    // Original VA
    };
    std::vector<RegionMarker> regions;  // Where each region starts in the data
};

}