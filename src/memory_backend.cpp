#include "memory_backend.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <dirent.h>
#include <regex>

namespace Haywire {

MemoryBackend::MemoryBackend() : mappedData(nullptr), mappedSize(0), fd(-1) {
}

MemoryBackend::~MemoryBackend() {
    Unmap();
}

bool MemoryBackend::AutoDetect() {
    // Common locations where QEMU might put memory-backend files
    std::vector<std::string> searchPaths = {
        "/dev/shm/",      // RAM-backed tmpfs (preferred)
        "/tmp/",          // Temp directory
        "/var/tmp/",      // Alternative temp
        "./",             // Current directory
    };
    
    // Look for files matching QEMU memory patterns
    std::regex memPattern("(qemu|vm|haywire).*mem.*", std::regex_constants::icase);
    
    for (const auto& searchPath : searchPaths) {
        DIR* dir = opendir(searchPath.c_str());
        if (!dir) continue;
        
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            
            // Check if filename matches memory pattern
            if (std::regex_search(filename, memPattern)) {
                std::string fullPath = searchPath + filename;
                
                // Try to map it
                if (TryMapPath(fullPath)) {
                    closedir(dir);
                    // Auto-detected memory backend
                    return true;
                }
            }
        }
        closedir(dir);
    }
    
    // Also check for explicitly named memory-backend files from ps output
    FILE* ps = popen("ps aux | grep qemu | grep memory-backend-file", "r");
    if (ps) {
        char line[1024];
        while (fgets(line, sizeof(line), ps)) {
            // Parse for mem-path=/some/path
            std::string lineStr(line);
            size_t memPathPos = lineStr.find("mem-path=");
            if (memPathPos != std::string::npos) {
                size_t start = memPathPos + 9;  // Length of "mem-path="
                size_t end = lineStr.find_first_of(" ,", start);
                std::string memPath = lineStr.substr(start, end - start);
                
                if (TryMapPath(memPath)) {
                    pclose(ps);
                    // Found memory backend from QEMU cmdline
                    return true;
                }
            }
        }
        pclose(ps);
    }
    
    return false;
}

bool MemoryBackend::TryMapPath(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    
    // Sanity check - memory files should be large
    if (st.st_size < 1024*1024) {  // At least 1MB
        return false;
    }
    
    // Check if it's actually a regular file or device
    if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
        return false;
    }
    
    return MapMemoryBackend(path, st.st_size);
}

bool MemoryBackend::MapMemoryBackend(const std::string& path, size_t size) {
    Unmap();
    
    // Try O_RDWR first for better sharing, fall back to O_RDONLY
    fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
    }
    
    // Use MAP_SHARED to see live changes from QEMU
    int prot = (fd >= 0 && fcntl(fd, F_GETFL) & O_RDWR) ? (PROT_READ | PROT_WRITE) : PROT_READ;
    mappedData = (uint8_t*)mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
    if (mappedData == MAP_FAILED) {
        // Fallback to MAP_PRIVATE if MAP_SHARED fails
        mappedData = (uint8_t*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mappedData == MAP_FAILED) {
            close(fd);
            fd = -1;
            mappedData = nullptr;
            return false;
        }
        // Warning: Using MAP_PRIVATE, changes may not be visible
    } else {
        // Successfully mapped with MAP_SHARED - live updates enabled
    }
    
    mappedSize = size;
    backendPath = path;
    
    // Advise kernel about our access pattern
    madvise(mappedData, mappedSize, MADV_RANDOM);  // We'll jump around in memory
    
    return true;
}

bool MemoryBackend::Read(uint64_t gpa, size_t size, std::vector<uint8_t>& buffer) {
    // TEST: ARM64 guest RAM starts at 0x40000000
    const uint64_t TEST_RAM_BASE = 0x40000000;
    
    // Convert physical address to file offset
    uint64_t fileOffset = gpa;
    if (gpa >= TEST_RAM_BASE) {
        fileOffset = gpa - TEST_RAM_BASE;
        static int debugCount = 0;
        if (++debugCount <= 5) {
            std::cerr << "MemoryBackend: PA 0x" << std::hex << gpa 
                      << " -> file offset 0x" << fileOffset << std::dec << std::endl;
        }
    }
    
    if (!mappedData || fileOffset >= mappedSize) {
        return false;
    }
    
    size_t available = mappedSize - fileOffset;
    size_t toRead = std::min(size, available);
    
    #ifdef __APPLE__
    // On macOS, try to force cache invalidation before reading
    // MS_INVALIDATE should discard any cached pages
    msync(mappedData + fileOffset, toRead, MS_INVALIDATE | MS_SYNC);
    
    // Also try madvise to tell kernel we're about to read this
    madvise(mappedData + fileOffset, toRead, MADV_DONTNEED);
    madvise(mappedData + fileOffset, toRead, MADV_WILLNEED);
    #endif
    
    buffer.resize(toRead);
    memcpy(buffer.data(), mappedData + fileOffset, toRead);
    
    return true;
}

const uint8_t* MemoryBackend::GetDirectPointer(uint64_t gpa) const {
    // TEST: ARM64 guest RAM starts at 0x40000000
    const uint64_t TEST_RAM_BASE = 0x40000000;
    
    uint64_t fileOffset = gpa;
    if (gpa >= TEST_RAM_BASE) {
        fileOffset = gpa - TEST_RAM_BASE;
    }
    
    if (!mappedData || fileOffset >= mappedSize) {
        return nullptr;
    }
    return mappedData + fileOffset;
}

void MemoryBackend::Unmap() {
    if (mappedData && mappedSize > 0) {
        munmap(mappedData, mappedSize);
        mappedData = nullptr;
        mappedSize = 0;
    }
    
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    
    backendPath.clear();
}

}