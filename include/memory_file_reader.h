#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace Haywire {

// Simple memory-mapped file reader to replace BeaconReader's mmap functionality
// Cross-platform: uses mmap on POSIX systems, CreateFileMapping on Windows
class MemoryFileReader {
public:
    MemoryFileReader();
    ~MemoryFileReader();

    // Initialize with memory file path
    // Default path is platform-specific: /tmp/haywire-vm-mem on POSIX, %TEMP%\haywire-vm-mem on Windows
    bool Initialize(const std::string& memoryFilePath = "");

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
    std::string filePath;

#ifdef _WIN32
    HANDLE hFile;
    HANDLE hMapping;
#else
    int memoryFd;
#endif
};

} // namespace Haywire