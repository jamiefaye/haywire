#include "page_database.h"
#include "kernel_discovery_backend.h"
#include "memory_types.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>

namespace Haywire {

// Background scanner thread
class BackgroundScanner {
public:
    BackgroundScanner(PageDatabase* db, KernelDiscoveryBackend* backend, int intervalMs)
        : database(db), backend(backend), intervalMs(intervalMs), running(false) {}

    ~BackgroundScanner() {
        Stop();
    }

    void Start() {
        if (running) return;
        running = true;
        thread = std::thread(&BackgroundScanner::ScanLoop, this);
    }

    void Stop() {
        if (!running) return;
        running = false;
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    PageDatabase* database;
    KernelDiscoveryBackend* backend;
    int intervalMs;
    bool running;
    std::thread thread;

    void ScanLoop() {
        while (running) {
            database->ScanAllProcesses(backend);
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
    }
};

PageDatabase::PageDatabase()
    : ramBase(0), ramSize(0), numPages(0), scanner(nullptr) {
}

PageDatabase::~PageDatabase() {
    StopBackgroundScanning();
}

void PageDatabase::Initialize(uint64_t ramBase, uint64_t ramSize) {
    std::lock_guard<std::mutex> lock(mutex);

    this->ramBase = ramBase;
    this->ramSize = ramSize;
    this->numPages = ramSize / 4096;

    // Allocate page array
    pages.resize(numPages);

    // Initialize all pages as unattributed at their physical addresses
    for (size_t i = 0; i < numPages; i++) {
        pages[i].physicalAddr = IndexToPhys(i);
        pages[i].virtualAddr = 0;
        pages[i].pid = 0;
        pages[i].ownershipType = PageMetadata::UNATTRIBUTED;
        pages[i].flags = 0;
        pages[i].filename.clear();
    }

    virtualLookup.clear();
}

size_t PageDatabase::ScanAllProcesses(KernelDiscoveryBackend* backend) {
    if (!backend) return 0;

    std::lock_guard<std::mutex> lock(mutex);

    // Reset all pages to unattributed
    for (auto& page : pages) {
        page.virtualAddr = 0;
        page.pid = 0;
        page.ownershipType = PageMetadata::UNATTRIBUTED;
        page.flags = 0;
        page.filename.clear();
    }
    virtualLookup.clear();

    // Discover all processes
    std::vector<ProcessInfo> processes;
    if (!backend->DiscoverProcesses(processes)) {
        return 0;
    }

    size_t attributedCount = 0;

    // For each process, get its memory sections
    for (const auto& proc : processes) {
        std::vector<SectionEntry> sections;
        if (!backend->GetProcessMemorySections(proc.pid, sections)) {
            continue;
        }

        // For each section, mark pages as attributed
        for (const auto& section : sections) {
            // Convert ownership type from SectionEntry to PageMetadata
            PageMetadata::OwnershipType ownershipType;
            switch (section.ownership_type) {
                case 1: ownershipType = PageMetadata::ANONYMOUS; break;
                case 2: ownershipType = PageMetadata::FILE_BACKED; break;
                case 3: ownershipType = PageMetadata::SHARED_LIB; break;
                case 4: ownershipType = PageMetadata::EXECUTABLE; break;
                case 5: ownershipType = PageMetadata::STACK; break;
                case 6: ownershipType = PageMetadata::HEAP; break;
                case 7: ownershipType = PageMetadata::VDSO; break;
                case 8: ownershipType = PageMetadata::VVAR; break;
                default: ownershipType = PageMetadata::UNATTRIBUTED; break;
            }

            // Get filename
            std::string filename(section.path);

            // Iterate through all pages in this section
            // Note: We don't have PTEs yet, so we're just marking the VMA range
            // In future, we could walk PTEs to find which pages are actually mapped
            uint64_t vaStart = section.va_start;
            uint64_t vaEnd = section.va_end;

            for (uint64_t va = vaStart; va < vaEnd; va += 4096) {
                // We don't have VA->PA translation yet (would need PTE walk)
                // For now, just build reverse lookup for VMA tracking
                // Physical attribution will be added when we integrate PTE walking

                uint64_t key = MakeVirtualKey(proc.pid, va);
                // Store section info for this virtual page
                // (We'll populate physical mapping when PTE walking is added)

                // For now, just track that this VA exists in this PID
                // This allows us to answer "what VMAs exist?" even without PA mapping
            }
        }
    }

    return attributedCount;
}

const PageMetadata* PageDatabase::GetPageByPhysical(uint64_t physAddr) const {
    std::lock_guard<std::mutex> lock(mutex);

    if (physAddr < ramBase || physAddr >= ramBase + ramSize) {
        return nullptr;
    }

    size_t index = PhysToIndex(physAddr);
    if (index >= pages.size()) {
        return nullptr;
    }

    return &pages[index];
}

const PageMetadata* PageDatabase::GetPageByVirtual(uint32_t pid, uint64_t virtAddr) const {
    std::lock_guard<std::mutex> lock(mutex);

    uint64_t key = MakeVirtualKey(pid, virtAddr);
    auto it = virtualLookup.find(key);
    if (it == virtualLookup.end()) {
        return nullptr;
    }

    size_t index = it->second;
    if (index >= pages.size()) {
        return nullptr;
    }

    return &pages[index];
}

PageDatabase::Stats PageDatabase::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex);

    Stats stats;
    stats.totalPages = numPages;
    stats.attributedPages = 0;
    stats.unattributedPages = 0;

    for (const auto& page : pages) {
        if (page.isAttributed()) {
            stats.attributedPages++;
            stats.pagesByType[page.ownershipType]++;
            stats.pagesByPID[page.pid]++;
        } else {
            stats.unattributedPages++;
        }
    }

    return stats;
}

void PageDatabase::Clear() {
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& page : pages) {
        page.virtualAddr = 0;
        page.pid = 0;
        page.ownershipType = PageMetadata::UNATTRIBUTED;
        page.flags = 0;
        page.filename.clear();
    }

    virtualLookup.clear();
}

void PageDatabase::StartBackgroundScanning(KernelDiscoveryBackend* backend, int intervalMs) {
    StopBackgroundScanning();

    scanner = std::make_unique<BackgroundScanner>(this, backend, intervalMs);
    scanner->Start();
}

void PageDatabase::StopBackgroundScanning() {
    if (scanner) {
        scanner->Stop();
        scanner.reset();
    }
}

} // namespace Haywire
