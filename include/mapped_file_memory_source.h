#pragma once

#include "memory_data_source.h"
#include <string>
#include <vector>

namespace Haywire {

// Memory-mapped file data source for large files
class MappedFileMemorySource : public MemoryDataSource {
public:
    MappedFileMemorySource();
    ~MappedFileMemorySource();

    // Open a file with memory mapping
    bool OpenFile(const std::string& filename);
    void Close();

    // MemoryDataSource interface
    bool ReadMemory(uint64_t address, uint8_t* buffer, size_t size) override;
    uint64_t GetMemorySize() const override;
    bool IsValidAddress(uint64_t address, size_t size) const override;
    std::string GetSourceName() const override;
    std::vector<MemoryRegion> GetMemoryRegions() const override;
    bool IsAvailable() const override;

private:
    std::string filename_;
    void* mapped_data_ = nullptr;
    size_t file_size_ = 0;
    int fd_ = -1;  // File descriptor
};

} // namespace Haywire