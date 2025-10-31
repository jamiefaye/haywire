#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "platform_compat.h"

namespace Haywire {

class KernelDiscoveryBackend;

// Memory reader that uses kernel discovery for VA→PA translation
class KernelMemoryReader {
public:
    KernelMemoryReader();
    ~KernelMemoryReader();

    // Initialize with memory file and kernel discovery backend
    bool Initialize(const std::string& memoryFilePath,
                   std::shared_ptr<KernelDiscoveryBackend> kernelDiscovery);

    // Clean up
    void Cleanup();

    // Read memory at a virtual address for a specific process
    // Uses kernel discovery's PTEs for VA→PA translation
    bool ReadVirtualMemory(uint32_t pid, uint64_t virtualAddr,
                          size_t size, std::vector<uint8_t>& buffer);

    // Read physical memory directly (for kernel structures)
    bool ReadPhysicalMemory(uint64_t physicalAddr, size_t size,
                           std::vector<uint8_t>& buffer);

    // Translate virtual address to physical using kernel discovery PTEs
    uint64_t TranslateVirtualToPhysical(uint32_t pid, uint64_t virtualAddr);

    // Get direct memory pointer for a physical address (if within mapped range)
    const uint8_t* GetPhysicalPointer(uint64_t physicalAddr) const;

    // Get size of mapped memory
    size_t GetMemorySize() const { return memorySize; }

    // Check if initialized
    bool IsInitialized() const { return memoryBase != nullptr; }

private:
    uint8_t* memoryBase;
    size_t memorySize;
    int memoryFd;
    std::string filePath;
    std::shared_ptr<KernelDiscoveryBackend> kernelDiscovery;

    // Guest RAM base address (usually 0x40000000 for ARM64)
    static constexpr uint64_t GUEST_RAM_BASE = 0x40000000;
};

} // namespace Haywire