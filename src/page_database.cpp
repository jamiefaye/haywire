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

size_t PageDatabase::ScanAllProcesses(KernelDiscoveryBackend* backend, int numThreads) {
    if (!backend) return 0;

    auto startTime = std::chrono::steady_clock::now();

    // Auto-detect number of threads if requested
    if (numThreads <= 0) {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads <= 0) numThreads = 4;  // Fallback
    }

    // Discover all processes (single-threaded, relatively fast)
    std::vector<ProcessInfo> processes;
    {
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

        if (!backend->DiscoverProcesses(processes)) {
            return 0;
        }
    }

    if (processes.empty()) {
        return 0;
    }

    std::cout << "Scanning " << processes.size()
              << " processes using " << numThreads << " threads\n";

    // Divide work among threads
    std::vector<std::thread> workers;
    std::atomic<size_t> totalAttributed{0};

    size_t perThread = (processes.size() + numThreads - 1) / numThreads;

    for (int t = 0; t < numThreads; t++) {
        size_t start = t * perThread;
        size_t end = std::min(start + perThread, processes.size());

        if (start >= processes.size()) break;

        workers.emplace_back([this, backend, &processes, start, end, &totalAttributed]() {
            size_t threadAttributed = ScanProcessRange(backend, processes, start, end);
            totalAttributed += threadAttributed;
        });
    }

    // Wait for all threads to complete
    for (auto& worker : workers) {
        worker.join();
    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "Attributed " << totalAttributed << " pages in "
              << duration << "ms using " << workers.size() << " threads\n";

    return totalAttributed;
}

// Scan a range of processes (called by worker thread)
size_t PageDatabase::ScanProcessRange(KernelDiscoveryBackend* backend,
                                     const std::vector<ProcessInfo>& processes,
                                     size_t start, size_t end) {
    size_t attributed = 0;

    for (size_t i = start; i < end; i++) {
        const auto& proc = processes[i];

        // Get sections and PTEs for this process
        std::vector<SectionEntry> sections;
        if (!backend->GetProcessMemorySections(proc.pid, sections)) {
            continue;
        }

        std::unordered_map<uint64_t, uint64_t> ptes;
        backend->GetProcessPTEs(proc.pid, ptes);

        // Attribute pages for this process
        attributed += AttributeProcessPages(proc, sections, ptes);
    }

    return attributed;
}

// Attribute pages for one process (thread-safe with batch updates)
size_t PageDatabase::AttributeProcessPages(const ProcessInfo& proc,
                                          const std::vector<SectionEntry>& sections,
                                          const std::unordered_map<uint64_t, uint64_t>& ptes) {
    // Build updates locally (no locks needed)
    struct Update {
        size_t pageIndex;
        PageMetadata meta;
        uint64_t virtualKey;
    };
    std::vector<Update> updates;

    for (const auto& section : sections) {
        PageMetadata::OwnershipType ownershipType = ConvertOwnershipType(section.ownership_type);
        std::string filename(section.path);
        uint32_t flags = ConvertFlags(section.perms);

        // Walk pages in this section
        for (uint64_t va = section.va_start; va < section.va_end; va += 4096) {
            auto it = ptes.find(va);
            if (it == ptes.end()) continue;

            uint64_t pa = it->second;
            if (pa < ramBase || pa >= ramBase + ramSize) continue;

            size_t pageIndex = PhysToIndex(pa);
            if (pageIndex >= pages.size()) continue;

            PageMetadata meta;
            meta.physicalAddr = pa;
            meta.virtualAddr = va;
            meta.pid = proc.pid;
            meta.ownershipType = ownershipType;
            meta.flags = flags;
            meta.filename = filename;

            uint64_t key = MakeVirtualKey(proc.pid, va);
            updates.push_back({pageIndex, meta, key});
        }
    }

    // Apply all updates with single lock (batch update)
    if (!updates.empty()) {
        std::lock_guard<std::mutex> lock(mutex);

        for (const auto& update : updates) {
            pages[update.pageIndex] = update.meta;
            virtualLookup[update.virtualKey] = update.pageIndex;
        }
    }

    return updates.size();
}

// Helper: Convert section ownership type to PageMetadata type
PageMetadata::OwnershipType PageDatabase::ConvertOwnershipType(uint32_t sectionType) {
    switch (sectionType) {
        case 1: return PageMetadata::ANONYMOUS;
        case 2: return PageMetadata::FILE_BACKED;
        case 3: return PageMetadata::SHARED_LIB;
        case 4: return PageMetadata::EXECUTABLE;
        case 5: return PageMetadata::STACK;
        case 6: return PageMetadata::HEAP;
        case 7: return PageMetadata::VDSO;
        case 8: return PageMetadata::VVAR;
        default: return PageMetadata::UNATTRIBUTED;
    }
}

// Helper: Convert section permissions to flags
uint32_t PageDatabase::ConvertFlags(uint32_t sectionPerms) {
    uint32_t flags = 0;
    if (sectionPerms & 0x01) flags |= 0x01;  // VM_READ
    if (sectionPerms & 0x02) flags |= 0x02;  // VM_WRITE
    if (sectionPerms & 0x04) flags |= 0x04;  // VM_EXEC
    if (sectionPerms & 0x08) flags |= 0x08;  // VM_SHARED
    return flags;
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
