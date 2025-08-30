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
                    std::cerr << "Auto-detected memory backend: " << fullPath 
                              << " (" << mappedSize / (1024*1024) << " MB)\n";
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
                    std::cerr << "Found memory backend from QEMU cmdline: " << memPath 
                              << " (" << mappedSize / (1024*1024) << " MB)\n";
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
    
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    
    // Try to map the entire guest memory
    mappedData = (uint8_t*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mappedData == MAP_FAILED) {
        close(fd);
        fd = -1;
        mappedData = nullptr;
        return false;
    }
    
    mappedSize = size;
    backendPath = path;
    
    // Advise kernel about our access pattern
    madvise(mappedData, mappedSize, MADV_RANDOM);  // We'll jump around in memory
    
    return true;
}

bool MemoryBackend::Read(uint64_t gpa, size_t size, std::vector<uint8_t>& buffer) {
    if (!mappedData || gpa >= mappedSize) {
        return false;
    }
    
    size_t available = mappedSize - gpa;
    size_t toRead = std::min(size, available);
    
    buffer.resize(toRead);
    memcpy(buffer.data(), mappedData + gpa, toRead);
    
    return true;
}

const uint8_t* MemoryBackend::GetDirectPointer(uint64_t gpa) const {
    if (!mappedData || gpa >= mappedSize) {
        return nullptr;
    }
    return mappedData + gpa;
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