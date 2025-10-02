#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "kernel_discovery_backend.h"

namespace Haywire {

// ViewportTranslator implementation using KernelDiscoveryBackend
// Provides VA to PA translation using kernel-discovered PTEs
class KernelViewportTranslator {
public:
    KernelViewportTranslator(std::shared_ptr<KernelDiscoveryBackend> backend);
    ~KernelViewportTranslator();

    // Set current viewport (compatibility with ViewportTranslator interface)
    void SetViewport(int pid, uint64_t centerVA, size_t viewSize);

    // Translate single address using kernel discovery
    uint64_t TranslateAddress(int pid, uint64_t virtualAddr);

    // Clear cache for specific process or all
    void ClearCache(int pid = -1);

    // Direct access to kernel discovery (for bypassing mutex in rendering)
    std::shared_ptr<KernelDiscoveryBackend> GetKernelDiscovery() const {
        return kernelDiscovery;
    }

private:
    std::shared_ptr<KernelDiscoveryBackend> kernelDiscovery;

    // Current viewport
    int currentPid;
    uint64_t viewportCenter;
    size_t viewportSize;

    // Simple cache: [pid][va_page] -> pa (protected by mutex for thread safety)
    std::unordered_map<int, std::unordered_map<uint64_t, uint64_t>> cache;
    mutable std::mutex cacheMutex;

    // Constants
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;

    // Helper to get page-aligned address
    uint64_t AlignToPage(uint64_t addr) const {
        return addr & ~PAGE_MASK;
    }
};


}