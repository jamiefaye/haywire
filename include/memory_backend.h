#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sys/mman.h>
#include <memory>

namespace Haywire {

// Forward declaration
class MemoryMapper;

class MemoryBackend {
public:
    MemoryBackend();
    ~MemoryBackend();
    
    // Try to find and map QEMU memory-backend file
    bool AutoDetect();
    
    // Directly map a memory-backend file
    bool MapMemoryBackend(const std::string& path, size_t size);
    
    // Read from mapped memory (guest physical address)
    bool Read(uint64_t gpa, size_t size, std::vector<uint8_t>& buffer);
    
    // Get direct pointer for zero-copy access
    const uint8_t* GetDirectPointer(uint64_t gpa) const;
    
    // Check if backend is available
    bool IsAvailable() const { return mappedData != nullptr; }
    
    // Get backend info
    std::string GetBackendPath() const { return backendPath; }
    size_t GetMappedSize() const { return mappedSize; }
    
    // Initialize memory mapping discovery
    bool InitializeMemoryMapping(const std::string& monitor_host = "localhost", 
                                 int monitor_port = 4444);
    
    void Unmap();
    
private:
    uint8_t* mappedData;
    size_t mappedSize;
    int fd;
    std::string backendPath;
    std::unique_ptr<MemoryMapper> memoryMapper;
    
    // Try common locations for memory-backend files
    bool TryMapPath(const std::string& path);
};

}