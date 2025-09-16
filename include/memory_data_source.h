#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace Haywire {

// Abstract interface for memory data sources
// This allows the visualizer to work with different data sources:
// - QEMU VM memory
// - Loaded binary files
// - Core dumps
// - Memory snapshots
class MemoryDataSource {
public:
    virtual ~MemoryDataSource() = default;

    // Read memory at the specified address
    // Returns true on success, false on failure
    virtual bool ReadMemory(uint64_t address, uint8_t* buffer, size_t size) = 0;

    // Get the total size of available memory
    virtual uint64_t GetMemorySize() const = 0;

    // Check if an address range is valid/readable
    virtual bool IsValidAddress(uint64_t address, size_t size) const = 0;

    // Get a descriptive name for this data source
    virtual std::string GetSourceName() const = 0;

    // Optional: Get memory regions/segments if available
    struct MemoryRegion {
        uint64_t start;
        uint64_t end;
        std::string name;
        std::string permissions;
    };
    virtual std::vector<MemoryRegion> GetMemoryRegions() const {
        return {}; // Default: no regions
    }

    // Optional: Support for address translation (VA to PA)
    virtual bool TranslateAddress(uint64_t virtualAddress, uint64_t& physicalAddress) {
        physicalAddress = virtualAddress; // Default: no translation
        return true;
    }

    // Check if this data source is currently connected/available
    virtual bool IsAvailable() const = 0;
};

// Shared pointer type for convenience
using MemoryDataSourcePtr = std::shared_ptr<MemoryDataSource>;

} // namespace Haywire