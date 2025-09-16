#include "file_memory_source.h"
#include <cstring>
#include <algorithm>

namespace Haywire {

FileMemorySource::FileMemorySource(const std::string& filename,
                                   std::shared_ptr<std::vector<uint8_t>> data)
    : filename_(filename), data_(data) {
    // Create a default region for the entire file if no regions are added
    if (data_ && !data_->empty()) {
        MemoryRegion defaultRegion;
        defaultRegion.start = 0;
        defaultRegion.end = data_->size();
        defaultRegion.name = filename;
        defaultRegion.permissions = "r---";
        regions_.push_back(defaultRegion);
    }
}

bool FileMemorySource::ReadMemory(uint64_t address, uint8_t* buffer, size_t size) {
    if (!data_ || !buffer) {
        return false;
    }

    // Check if the requested range is within the file
    if (address >= data_->size()) {
        return false;
    }

    // Calculate how much we can actually read
    size_t available = data_->size() - address;
    size_t toRead = std::min(size, available);

    // Copy the data
    std::memcpy(buffer, data_->data() + address, toRead);

    // If we couldn't read the full requested size, zero out the rest
    if (toRead < size) {
        std::memset(buffer + toRead, 0, size - toRead);
    }

    return true;
}

uint64_t FileMemorySource::GetMemorySize() const {
    return data_ ? data_->size() : 0;
}

bool FileMemorySource::IsValidAddress(uint64_t address, size_t size) const {
    if (!data_) {
        return false;
    }

    // Check for overflow
    if (address + size < address) {
        return false;
    }

    return address + size <= data_->size();
}

std::string FileMemorySource::GetSourceName() const {
    return "File: " + filename_;
}

std::vector<FileMemorySource::MemoryRegion> FileMemorySource::GetMemoryRegions() const {
    return regions_;
}

bool FileMemorySource::IsAvailable() const {
    return data_ && !data_->empty();
}

void FileMemorySource::AddRegion(const MemoryRegion& region) {
    regions_.push_back(region);
}

void FileMemorySource::ClearRegions() {
    regions_.clear();
}

} // namespace Haywire