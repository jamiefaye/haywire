#include "kernel_viewport_translator.h"
#include <iostream>

namespace Haywire {

KernelViewportTranslator::KernelViewportTranslator(std::shared_ptr<KernelDiscoveryBackend> backend)
    : kernelDiscovery(backend), currentPid(-1), viewportCenter(0), viewportSize(0) {
}

KernelViewportTranslator::~KernelViewportTranslator() {
}

void KernelViewportTranslator::SetViewport(int pid, uint64_t centerVA, size_t viewSize) {
    // Check if we switched processes
    if (pid != currentPid && pid > 0) {
        // Clear cache when switching processes
        cache[pid].clear();

        // Make sure kernel discovery has this PID selected
        if (kernelDiscovery) {
            kernelDiscovery->SelectProcess(pid);
        }
    }

    currentPid = pid;
    viewportCenter = centerVA;
    viewportSize = viewSize;
}

uint64_t KernelViewportTranslator::TranslateAddress(int pid, uint64_t virtualAddr) {
    if (!kernelDiscovery) {
        return 0;
    }

    // Get page-aligned address
    uint64_t pageAddr = AlignToPage(virtualAddr);
    uint64_t pageOffset = virtualAddr & PAGE_MASK;

    // Check cache first
    auto pidIt = cache.find(pid);
    if (pidIt != cache.end()) {
        auto pageIt = pidIt->second.find(pageAddr);
        if (pageIt != pidIt->second.end()) {
            // Cache hit - add offset within page
            if (pageIt->second != 0) {
                return pageIt->second + pageOffset;
            }
            return 0; // Cached as not present
        }
    }

    // Cache miss - ensure correct PID is selected
    if (pid != currentPid || currentPid < 0) {
        kernelDiscovery->SelectProcess(pid);
        currentPid = pid;
    }

    // Translate using kernel discovery
    uint64_t physAddr = kernelDiscovery->TranslateVA(virtualAddr);

    // Cache the page translation (store page base address)
    if (physAddr != 0) {
        uint64_t physPageAddr = physAddr & ~PAGE_MASK;
        cache[pid][pageAddr] = physPageAddr;
    } else {
        // Cache negative result too
        cache[pid][pageAddr] = 0;
    }

    return physAddr;
}

void KernelViewportTranslator::ClearCache(int pid) {
    if (pid == -1) {
        cache.clear();
    } else {
        cache[pid].clear();
    }
}

}