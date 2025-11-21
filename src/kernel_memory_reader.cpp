#include "kernel_memory_reader.h"
#include "kernel_discovery_backend.h"
#include "platform_compat.h"
#include <iostream>
#include <sys/stat.h>
#include <cstring>

namespace Haywire {

KernelMemoryReader::KernelMemoryReader()
    : memoryBase(nullptr), memorySize(0), memoryFd(-1) {
}

KernelMemoryReader::~KernelMemoryReader() {
    Cleanup();
}

bool KernelMemoryReader::Initialize(const std::string& memoryFilePath,
                                   std::shared_ptr<KernelDiscoveryBackend> discovery) {
    if (memoryBase) {
        std::cerr << "KernelMemoryReader already initialized\n";
        return true;
    }

    kernelDiscovery = discovery;
    if (!kernelDiscovery) {
        std::cerr << "KernelMemoryReader: No kernel discovery backend provided\n";
        return false;
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
    std::cout << "KernelMemoryReader: Memory file size: " << (memorySize / (1024*1024)) << " MB\n";

    // Memory map the file
    memoryBase = (uint8_t*)mmap(nullptr, memorySize, PROT_READ, MAP_PRIVATE, memoryFd, 0);
    if (memoryBase == MAP_FAILED) {
        std::cerr << "Failed to mmap memory file\n";
        close(memoryFd);
        memoryFd = -1;
        memoryBase = nullptr;
        return false;
    }

    std::cout << "KernelMemoryReader: Successfully mapped memory file at " << (void*)memoryBase << "\n";
    return true;
}

void KernelMemoryReader::Cleanup() {
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

bool KernelMemoryReader::ReadVirtualMemory(uint32_t pid, uint64_t virtualAddr,
                                          size_t size, std::vector<uint8_t>& buffer) {
    if (!kernelDiscovery || !memoryBase) {
        return false;
    }

    buffer.clear();
    buffer.reserve(size);

    // Read page by page, translating VA→PA for each page
    while (buffer.size() < size) {
        // Calculate offset within current page
        uint64_t pageOffset = virtualAddr & 0xFFF;  // 4KB pages
        uint64_t pageVA = virtualAddr & ~0xFFF;
        size_t bytesToRead = std::min(size - buffer.size(), (size_t)(0x1000 - pageOffset));

        // Translate VA to PA using kernel discovery
        uint64_t physAddr = TranslateVirtualToPhysical(pid, pageVA);
        if (physAddr == 0) {
            // Translation failed - might be unmapped page
            // Fill with zeros
            buffer.resize(buffer.size() + bytesToRead, 0);
        } else {
            // Add page offset to physical address
            physAddr += pageOffset;

            // Read from physical memory
            std::vector<uint8_t> pageData;
            if (ReadPhysicalMemory(physAddr, bytesToRead, pageData)) {
                buffer.insert(buffer.end(), pageData.begin(), pageData.end());
            } else {
                // Read failed - fill with zeros
                buffer.resize(buffer.size() + bytesToRead, 0);
            }
        }

        virtualAddr += bytesToRead;
    }

    return true;
}

bool KernelMemoryReader::ReadPhysicalMemory(uint64_t physicalAddr, size_t size,
                                           std::vector<uint8_t>& buffer) {
    buffer.clear();

    // Check if this is in the guest RAM range (mapped in our file)
    if (physicalAddr >= GUEST_RAM_BASE && physicalAddr < GUEST_RAM_BASE + memorySize) {
        // Convert physical address to file offset
        uint64_t fileOffset = physicalAddr - GUEST_RAM_BASE;

        if (fileOffset + size <= memorySize) {
            buffer.resize(size);
            std::memcpy(buffer.data(), memoryBase + fileOffset, size);
            return true;
        }
    }

    // Physical address is outside mapped range
    // Could use QMP fallback here if needed
    return false;
}

uint64_t KernelMemoryReader::TranslateVirtualToPhysical(uint32_t pid, uint64_t virtualAddr) {
    if (!kernelDiscovery) {
        return 0;
    }

    // Select the process for translation
    kernelDiscovery->SelectProcess(pid);

    // Use kernel discovery's TranslateVA method
    return kernelDiscovery->TranslateVA(virtualAddr);
}

const uint8_t* KernelMemoryReader::GetPhysicalPointer(uint64_t physicalAddr) const {
    if (!memoryBase) {
        return nullptr;
    }

    // Check if this is in the guest RAM range
    if (physicalAddr >= GUEST_RAM_BASE && physicalAddr < GUEST_RAM_BASE + memorySize) {
        uint64_t fileOffset = physicalAddr - GUEST_RAM_BASE;
        return memoryBase + fileOffset;
    }

    return nullptr;
}

} // namespace Haywire