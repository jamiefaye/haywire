#pragma once

#include <cstdint>
#include <vector>
#include <memory>

namespace Haywire {

class MemoryBackend;

// Abstract base class for page table walking
// Platform-specific implementations will handle ARM64, x86-64, etc.
class PageWalker {
public:
    PageWalker(MemoryBackend* backend) : memory(backend) {}
    virtual ~PageWalker() = default;

    // Set the page table base register(s) for a process
    // ARM64: TTBR0/TTBR1
    // x86-64: CR3
    virtual void SetPageTableBase(uint64_t base0, uint64_t base1 = 0) = 0;

    // Walk page tables to translate VA to PA
    virtual uint64_t TranslateAddress(uint64_t virtualAddr) = 0;

    // Bulk translate a range
    virtual size_t TranslateRange(uint64_t startVA, size_t numPages,
                                  std::vector<uint64_t>& physAddrs) = 0;

    // Get the page size for this architecture
    virtual uint64_t GetPageSize() const = 0;

    // Get architecture name for debugging
    virtual const char* GetArchitectureName() const = 0;

protected:
    MemoryBackend* memory;

    // Read 64-bit value from guest physical memory
    uint64_t ReadPhys64(uint64_t paddr);

    // Read 32-bit value from guest physical memory
    uint32_t ReadPhys32(uint64_t paddr);
};

// Factory function to create appropriate page walker
std::unique_ptr<PageWalker> CreatePageWalker(MemoryBackend* backend,
                                              const std::string& arch);

}