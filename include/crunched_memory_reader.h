#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "address_space_flattener.h"
#include "viewport_translator.h"
#include "beacon_translator.h"
#include "qemu_connection.h"

namespace Haywire {

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
    void SetBeaconTranslator(std::shared_ptr<BeaconTranslator> beaconTranslator) {
        this->beaconTranslator = beaconTranslator;
    }
    void SetConnection(QemuConnection* qemu) {
        this->qemu = qemu;
    }
    QemuConnection* GetConnection() const { return qemu; }
    void SetPID(int pid) { 
        this->targetPid = pid; 
    }
    
    // Read from crunched address space
    // flatAddress: Position in flattened space (0 to totalMappedSize)
    // size: Number of bytes to read
    // Returns actual bytes read
    size_t ReadCrunchedMemory(uint64_t flatAddress, size_t size, 
                             std::vector<uint8_t>& buffer);
    
    // Get total size of crunched space
    uint64_t GetCrunchedSize() const {
        return flattener ? flattener->GetFlatSize() : 0;
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
    std::shared_ptr<BeaconTranslator> beaconTranslator;
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