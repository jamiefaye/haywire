#pragma once

#include <cstdint>
#include <string>
#include <sys/mman.h>

namespace Haywire {

// Simple memory-mapped file reader to replace BeaconReader's mmap functionality
class MemoryFileReader {
public:
    MemoryFileReader();
    ~MemoryFileReader();

    // Initialize with memory file path
    bool Initialize(const std::string& memoryFilePath = "/tmp/haywire-vm-mem");

    // Clean up
    void Cleanup();

    // Get direct memory pointer for a given offset
    const uint8_t* GetMemoryPointer(size_t offset) const {
        if (!memoryBase || offset >= memorySize) {
            return nullptr;
        }
        return memoryBase + offset;
    }

    // Get size of mapped memory
    size_t GetMemorySize() const { return memorySize; }

    // Check if initialized
    bool IsInitialized() const { return memoryBase != nullptr; }

private:
    uint8_t* memoryBase;
    size_t memorySize;
    int memoryFd;
    std::string filePath;
};

} // namespace Haywire