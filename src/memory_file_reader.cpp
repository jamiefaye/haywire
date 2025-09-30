#include "memory_file_reader.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace Haywire {

MemoryFileReader::MemoryFileReader()
    : memoryBase(nullptr), memorySize(0), memoryFd(-1) {
}

MemoryFileReader::~MemoryFileReader() {
    Cleanup();
}

bool MemoryFileReader::Initialize(const std::string& memoryFilePath) {
    if (memoryBase) {
        std::cerr << "MemoryFileReader already initialized\n";
        return true;
    }

    filePath = memoryFilePath;

    // Open the memory file
    memoryFd = open(filePath.c_str(), O_RDONLY);
    if (memoryFd < 0) {
        std::cerr << "Failed to open memory file: " << filePath << "\n";
        return false;
    }

    // Get file size
    struct stat st;
    if (fstat(memoryFd, &st) < 0) {
        std::cerr << "Failed to stat memory file\n";
        close(memoryFd);
        memoryFd = -1;
        return false;
    }

    memorySize = st.st_size;
    std::cout << "Memory file size: " << (memorySize / (1024*1024)) << " MB\n";

    // Memory map the file (MAP_SHARED to see live updates from QEMU)
    memoryBase = (uint8_t*)mmap(nullptr, memorySize, PROT_READ, MAP_SHARED, memoryFd, 0);
    if (memoryBase == MAP_FAILED) {
        std::cerr << "Failed to mmap memory file\n";
        close(memoryFd);
        memoryFd = -1;
        memoryBase = nullptr;
        return false;
    }

    std::cout << "Successfully mapped memory file at " << (void*)memoryBase << "\n";
    return true;
}

void MemoryFileReader::Cleanup() {
    if (memoryBase && memoryBase != MAP_FAILED) {
        munmap(memoryBase, memorySize);
        memoryBase = nullptr;
    }

    if (memoryFd >= 0) {
        close(memoryFd);
        memoryFd = -1;
    }

    memorySize = 0;
}

} // namespace Haywire