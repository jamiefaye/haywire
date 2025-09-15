#include "platform/page_walker.h"
#include "platform/arm64/arm64_page_walker.h"
#include "platform/x86_64/x86_64_page_walker.h"
#include "memory_backend.h"
#include <cstring>
#include <vector>

namespace Haywire {

uint64_t PageWalker::ReadPhys64(uint64_t paddr) {
    uint64_t value = 0;
    if (memory && memory->IsAvailable()) {
        std::vector<uint8_t> buffer;
        if (memory->Read(paddr, 8, buffer) && buffer.size() == 8) {
            // Little-endian
            memcpy(&value, buffer.data(), 8);
        }
    }
    return value;
}

uint32_t PageWalker::ReadPhys32(uint64_t paddr) {
    uint32_t value = 0;
    if (memory && memory->IsAvailable()) {
        std::vector<uint8_t> buffer;
        if (memory->Read(paddr, 4, buffer) && buffer.size() == 4) {
            // Little-endian
            memcpy(&value, buffer.data(), 4);
        }
    }
    return value;
}

std::unique_ptr<PageWalker> CreatePageWalker(MemoryBackend* backend,
                                             const std::string& arch) {
    if (arch == "arm64" || arch == "aarch64") {
        return std::make_unique<ARM64PageWalker>(backend);
    } else if (arch == "x86_64" || arch == "x64" || arch == "amd64") {
        return std::make_unique<X86_64PageWalker>(backend);
    }

    // Default to x86-64 for Intel hardware
    return std::make_unique<X86_64PageWalker>(backend);
}

}