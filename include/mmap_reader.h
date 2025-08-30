#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sys/mman.h>

namespace Haywire {

class MMapReader {
public:
    MMapReader();
    ~MMapReader();
    
    // Dump memory via QMP then mmap it
    bool DumpAndMap(class QemuConnection& qemu, uint64_t address, size_t size);
    
    // Direct mmap of existing file
    bool MapFile(const std::string& path, size_t size);
    
    // Read from mapped memory
    bool Read(uint64_t offset, size_t size, std::vector<uint8_t>& buffer);
    
    // Get pointer to mapped memory
    const uint8_t* GetData() const { return mappedData; }
    size_t GetSize() const { return mappedSize; }
    
    void Unmap();
    
private:
    uint8_t* mappedData;
    size_t mappedSize;
    int fd;
};

}