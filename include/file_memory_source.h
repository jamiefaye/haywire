#pragma once

#include "memory_data_source.h"
#include <vector>
#include <memory>

namespace Haywire {

// Memory data source for loaded binary files
class FileMemorySource : public MemoryDataSource {
public:
    FileMemorySource(const std::string& filename,
                     std::shared_ptr<std::vector<uint8_t>> data);

    // MemoryDataSource interface
    bool ReadMemory(uint64_t address, uint8_t* buffer, size_t size) override;
    uint64_t GetMemorySize() const override;
    bool IsValidAddress(uint64_t address, size_t size) const override;
    std::string GetSourceName() const override;
    std::vector<MemoryRegion> GetMemoryRegions() const override;
    bool IsAvailable() const override;

    // Add memory regions (e.g., from parsed segments)
    void AddRegion(const MemoryRegion& region);
    void ClearRegions();

private:
    std::string filename_;
    std::shared_ptr<std::vector<uint8_t>> data_;
    std::vector<MemoryRegion> regions_;
};

} // namespace Haywire