#include "page_database.h"
#include "kernel_discovery_backend.h"
#include "memory_types.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <queue>
#include <unordered_set>
#include <condition_variable>
#include <atomic>
#include <iostream>

namespace Haywire {

// Background scanner thread with priority queue
class BackgroundScanner {
public:
    BackgroundScanner(PageDatabase* db, KernelDiscoveryBackend* backend, int rescanIntervalSec = 30)
        : database(db), backend(backend), rescanIntervalSec(rescanIntervalSec), running(false) {}

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
        cv.notify_all();  // Wake up thread if sleeping
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Add a PID to priority queue (scanned before background PIDs)
    void RequestPriorityScan(uint32_t pid) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (scannedPIDs.find(pid) == scannedPIDs.end()) {
            priorityQueue.push(pid);
            cv.notify_one();
        }
    }

    bool IsScanning() const { return running.load(); }

private:
    PageDatabase* database;
    KernelDiscoveryBackend* backend;
    int rescanIntervalSec;
    std::atomic<bool> running{false};
    std::thread thread;

    // Priority queue for on-demand PID scans
    std::queue<uint32_t> priorityQueue;
    std::unordered_set<uint32_t> scannedPIDs;  // Track what we've scanned
    std::mutex queueMutex;
    std::condition_variable cv;

    std::chrono::steady_clock::time_point lastFullScanTime;

    void ScanLoop() {
        std::cout << "[PageDB] Background scanner started (rescan every " << rescanIntervalSec << "s)\n";

        lastFullScanTime = std::chrono::steady_clock::now();

        // Get initial process list
        std::vector<ProcessInfo> allProcesses;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!backend->DiscoverProcesses(allProcesses)) {
                std::cerr << "[PageDB] Failed to discover processes\n";
                return;
            }
            database->totalProcessCount.store(allProcesses.size());
            std::cout << "[PageDB] Found " << allProcesses.size() << " total processes\n";
        }

        size_t backgroundIndex = 0;

        while (running) {
            uint32_t pidToScan = 0;
            bool isPriority = false;

            // Check priority queue first
            {
                std::unique_lock<std::mutex> lock(queueMutex);

                if (!priorityQueue.empty()) {
                    pidToScan = priorityQueue.front();
                    priorityQueue.pop();
                    isPriority = true;
                } else if (backgroundIndex < allProcesses.size()) {
                    // Continue background scan
                    pidToScan = allProcesses[backgroundIndex].pid;
                    backgroundIndex++;
                } else {
                    // All done - mark complete and check if we should rescan
                    if (!database->fullScanComplete.load()) {
                        database->fullScanComplete.store(true);
                        database->scanGeneration.fetch_add(1);  // Increment generation on completion
                        std::cout << "[PageDB] Full scan complete! Scanned "
                                  << database->scannedProcessCount.load() << " processes (generation "
                                  << database->scanGeneration.load() << ")\n";
                    }

                    // Check if it's time to rescan (skip if interval is 0)
                    if (rescanIntervalSec > 0) {
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - lastFullScanTime).count();

                        if (elapsed >= rescanIntervalSec) {
                        // Time to rescan - rediscover processes and start over
                        std::cout << "[PageDB] Starting periodic rescan (" << elapsed << "s elapsed)\n";

                        std::vector<ProcessInfo> newProcesses;
                        if (backend->DiscoverProcesses(newProcesses)) {
                            // Don't clear - just update as we rescan
                            allProcesses = std::move(newProcesses);
                            database->totalProcessCount.store(allProcesses.size());
                            backgroundIndex = 0;
                            scannedPIDs.clear();
                            database->scannedProcessCount.store(0);
                            // Keep fullScanComplete true - this is a rescan, not first scan
                            lastFullScanTime = now;
                            std::cout << "[PageDB] Rescan: updating database, found " << allProcesses.size() << " processes\n";
                            continue;  // Start scanning immediately
                        } else {
                            std::cerr << "[PageDB] Rescan failed to discover processes\n";
                        }
                        }
                    }

                    // Wait for priority requests or shutdown
                    cv.wait_for(lock, std::chrono::seconds(1));
                    continue;
                }
            }

            // Skip if already scanned
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (scannedPIDs.find(pidToScan) != scannedPIDs.end()) {
                    continue;
                }
            }

            // Find the ProcessInfo for this PID
            ProcessInfo procToScan;
            bool found = false;
            for (const auto& proc : allProcesses) {
                if (proc.pid == pidToScan) {
                    procToScan = proc;
                    found = true;
                    break;
                }
            }

            if (!found) continue;

            // Scan this process
            if (isPriority) {
                std::cout << "[PageDB] Priority scan PID " << pidToScan
                          << " (" << procToScan.name << ")\n";
            }

            size_t attributed = database->ScanSingleProcess(procToScan, backend);

            // Mark as scanned
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                scannedPIDs.insert(pidToScan);
                database->scannedProcessCount.fetch_add(1);
            }

            if (isPriority || (database->scannedProcessCount.load() % 10 == 0)) {
                std::cout << "[PageDB] Progress: " << database->scannedProcessCount.load()
                          << "/" << database->totalProcessCount.load()
                          << " processes (" << attributed << " pages from PID " << pidToScan << ")\n";
            }

            // Delay between PIDs to avoid monopolizing CPU
            // First scan: faster (10ms priority, 20ms background) to populate database quickly
            // Rescans: slower (50ms priority, 100ms background) to be CPU-friendly
            bool isFirstScan = (database->scanGeneration.load() == 0);
            int delayMs;
            if (isPriority) {
                delayMs = isFirstScan ? 10 : 50;
            } else {
                delayMs = isFirstScan ? 20 : 100;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

        std::cout << "[PageDB] Background scanner stopped\n";
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

    std::cout << "[PageDatabase::Initialize] ramBase=0x" << std::hex << ramBase
              << " ramSize=0x" << ramSize << std::dec
              << " (" << (ramSize / (1024.0 * 1024.0 * 1024.0)) << " GB)"
              << " numPages=" << numPages << "\n";

    // Allocate page array
    pages.resize(numPages);

    // Initialize all pages as unattributed at their physical addresses
    for (size_t i = 0; i < numPages; i++) {
        pages[i].physicalAddr = IndexToPhys(i);
        pages[i].virtualAddr = 0;
        pages[i].pids.clear();
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
            page.pids.clear();
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

    // PERFORMANCE FIX: Instead of iterating all possible VAs in each section and doing
    // hash lookups (O(address_space_size)), iterate the PTE map once and check which
    // section each page belongs to (O(actual_pages)). This is 100-1000x faster for
    // processes with large sparse address spaces.

    size_t physIndexFailed = 0;
    size_t noMatchingSection = 0;
    uint64_t minRejectedPA = UINT64_MAX;
    uint64_t maxRejectedPA = 0;
    size_t maxRejectedIndex = 0;

    for (const auto& [va, pa] : ptes) {
        // PhysToIndex() returns SIZE_MAX for invalid addresses (PCI hole, beyond RAM)
        size_t pageIndex = PhysToIndex(pa);
        if (pageIndex >= pages.size()) {
            physIndexFailed++;
            if (pa < minRejectedPA) minRejectedPA = pa;
            if (pa > maxRejectedPA) maxRejectedPA = pa;
            if (pageIndex != SIZE_MAX && pageIndex > maxRejectedIndex) maxRejectedIndex = pageIndex;
            continue;
        }

        // Find which section this VA belongs to
        const SectionEntry* matchingSection = nullptr;
        for (const auto& section : sections) {
            if (va >= section.va_start && va < section.va_end) {
                matchingSection = &section;
                break;
            }
        }

        if (!matchingSection) {
            noMatchingSection++;
            continue;
        }

        // Build metadata for this page
        PageMetadata meta;
        meta.physicalAddr = pa;
        meta.virtualAddr = va;
        meta.pids.push_back(proc.pid);
        meta.ownershipType = ConvertOwnershipType(matchingSection->ownership_type);
        meta.flags = ConvertFlags(matchingSection->perms);
        meta.filename = std::string(matchingSection->path);

        uint64_t key = MakeVirtualKey(proc.pid, va);
        updates.push_back({pageIndex, meta, key});
    }

    // DEBUG: Show filtering stats
    if (proc.pid == 4696) {
        std::cout << "[AttributeProcessPages] PID " << proc.pid << ": " << ptes.size() << " PTEs\n";
        std::cout << "[AttributeProcessPages]   PhysToIndex failed: " << physIndexFailed << "\n";
        if (physIndexFailed > 0) {
            std::cout << "[AttributeProcessPages]   Rejected PA range: 0x" << std::hex << minRejectedPA
                      << " - 0x" << maxRejectedPA << std::dec << "\n";
        }
        std::cout << "[AttributeProcessPages]   No matching section: " << noMatchingSection << "\n";
        std::cout << "[AttributeProcessPages]   Created updates: " << updates.size() << "\n";
    }

    // Apply all updates with single lock (batch update)
    if (!updates.empty()) {
        std::lock_guard<std::mutex> lock(mutex);

        for (const auto& update : updates) {
            PageMetadata& existing = pages[update.pageIndex];

            // If page is unattributed, use the new metadata entirely
            if (!existing.isAttributed()) {
                pages[update.pageIndex] = update.meta;
            } else {
                // Page already attributed - add this PID to the list if not already present
                existing.addProcess(proc.pid);
                // Keep first attribution's metadata (type, flags, filename)
            }

            virtualLookup[update.virtualKey] = update.pageIndex;
        }

        // Verify first few pages were actually set
        if (proc.pid == 3101 && updates.size() > 0) {
            std::cout << "[PageDB] Verification: PID 3101, updated " << updates.size() << " pages\n";
            const auto& firstPage = pages[updates[0].pageIndex];
            std::cout << "[PageDB] First page: index=" << updates[0].pageIndex
                      << " pids=[";
            for (size_t i = 0; i < firstPage.pids.size(); i++) {
                if (i > 0) std::cout << ",";
                std::cout << firstPage.pids[i];
            }
            std::cout << "] va=0x" << std::hex << firstPage.virtualAddr << std::dec << "\n";
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
            // Count page for each owning PID
            for (uint32_t pid : page.pids) {
                stats.pagesByPID[pid]++;
            }
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
        page.pids.clear();
        page.ownershipType = PageMetadata::UNATTRIBUTED;
        page.flags = 0;
        page.filename.clear();
    }

    virtualLookup.clear();
}

// Scan a single process (helper method)
size_t PageDatabase::ScanSingleProcess(const ProcessInfo& proc, KernelDiscoveryBackend* backend) {
    // Get sections and PTEs for this process
    std::vector<SectionEntry> sections;
    if (!backend->GetProcessMemorySections(proc.pid, sections)) {
        std::cout << "[PageDB] Failed to get sections for PID " << proc.pid << "\n";
        return 0;
    }

    std::unordered_map<uint64_t, uint64_t> ptes;
    backend->GetProcessPTEs(proc.pid, ptes);

    std::cout << "[PageDB] PID " << proc.pid << ": " << sections.size()
              << " sections, " << ptes.size() << " PTEs\n";

    // Attribute pages for this process
    size_t attributed = AttributeProcessPages(proc, sections, ptes);

    std::cout << "[PageDB] PID " << proc.pid << ": Attributed " << attributed << " pages\n";

    // Store process name for querying
    {
        std::lock_guard<std::mutex> lock(mutex);
        processNames[proc.pid] = proc.name;
    }

    // Mark this PID as scanned
    {
        std::lock_guard<std::mutex> lock(mutex);
        scannedPIDs.insert(proc.pid);
    }

    return attributed;
}

// Scan a single PID immediately (blocking)
size_t PageDatabase::ScanSinglePID(uint32_t pid, KernelDiscoveryBackend* backend) {
    if (!backend) return 0;

    // Get process info
    ProcessInfo proc;
    if (!backend->GetProcessInfo(pid, proc)) {
        return 0;
    }

    std::cout << "[PageDB] Immediate scan PID " << pid << " (" << proc.name << ")\n";
    return ScanSingleProcess(proc, backend);
}

// Request priority scan (non-blocking)
void PageDatabase::RequestPriorityScan(uint32_t pid) {
    if (scanner) {
        scanner->RequestPriorityScan(pid);
    }
}

// Get all pages for a specific PID
std::vector<const PageMetadata*> PageDatabase::GetPagesForPID(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex);

    std::vector<const PageMetadata*> result;
    for (const auto& page : pages) {
        if (page.hasProcess(pid)) {
            result.push_back(&page);
        }
    }
    return result;
}

// Get PID data in visualizer format
bool PageDatabase::GetPIDData(uint32_t pid,
                               std::vector<SectionEntry>& sections,
                               std::unordered_map<uint64_t, uint64_t>& ptes) const {
    sections.clear();
    ptes.clear();

    // Check if this PID has been scanned
    if (!IsPIDScanned(pid)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);

    // DEBUG: Check how many pages have this PID via old method
    size_t oldMethodCount = 0;
    for (const auto& page : pages) {
        if (page.hasProcess(pid)) {
            oldMethodCount++;
        }
    }
    std::cout << "[PageDB::GetPIDData] PID " << pid << ": Old method finds " << oldMethodCount << " pages with hasProcess()\n";
    std::cout << "[PageDB::GetPIDData] virtualLookup has " << virtualLookup.size() << " total entries\n";

    // Use virtualLookup to get CORRECT VAs for this PID (not the first process's VA!)
    // Each physical page can be mapped at different VAs by different PIDs (shared DLLs)
    struct PageWithVA {
        uint64_t virtualAddr;
        const PageMetadata* page;
    };
    std::vector<PageWithVA> pidPages;

    size_t virtualLookupMatches = 0;
    for (const auto& [key, pageIndex] : virtualLookup) {
        // Extract PID from key: (pid << 32) | (va >> 12)
        uint32_t keyPid = static_cast<uint32_t>(key >> 32);
        if (keyPid != pid) continue;
        virtualLookupMatches++;

        // Extract VA from key (restore the page-aligned address)
        uint64_t virtualAddr = (key & 0xFFFFFFFF) << 12;

        // Get the page metadata
        if (pageIndex >= pages.size()) continue;
        const PageMetadata* page = &pages[pageIndex];

        pidPages.push_back({virtualAddr, page});
    }

    std::cout << "[PageDB::GetPIDData] virtualLookup found " << virtualLookupMatches << " entries for PID " << pid << "\n";
    std::cout << "[PageDB::GetPIDData] After filtering, have " << pidPages.size() << " pages\n";

    if (pidPages.empty()) {
        return false;
    }

    // Sort by virtual address (now using CORRECT VA for this PID)
    std::sort(pidPages.begin(), pidPages.end(),
              [](const PageWithVA& a, const PageWithVA& b) {
                  return a.virtualAddr < b.virtualAddr;
              });

    // Build PTEs using CORRECT VAs for this PID
    for (const auto& entry : pidPages) {
        ptes[entry.virtualAddr] = entry.page->physicalAddr;
    }

    // Coalesce consecutive pages into sections (using CORRECT VAs)
    SectionEntry currentSection;
    currentSection.va_start = pidPages[0].virtualAddr;
    currentSection.va_end = pidPages[0].virtualAddr + 4096;
    currentSection.ownership_type = static_cast<uint32_t>(pidPages[0].page->ownershipType);
    currentSection.perms = pidPages[0].page->flags;
    strncpy(currentSection.path, pidPages[0].page->filename.c_str(), sizeof(currentSection.path) - 1);
    currentSection.path[sizeof(currentSection.path) - 1] = '\0';

    for (size_t i = 1; i < pidPages.size(); i++) {
        const auto& entry = pidPages[i];

        // Can we merge this page into current section?
        bool canMerge = (entry.virtualAddr == currentSection.va_end &&
                        entry.page->ownershipType == static_cast<PageMetadata::OwnershipType>(currentSection.ownership_type) &&
                        entry.page->flags == currentSection.perms &&
                        entry.page->filename == currentSection.path);

        if (canMerge) {
            // Extend current section
            currentSection.va_end = entry.virtualAddr + 4096;
        } else {
            // Save current section and start new one
            sections.push_back(currentSection);

            currentSection.va_start = entry.virtualAddr;
            currentSection.va_end = entry.virtualAddr + 4096;
            currentSection.ownership_type = static_cast<uint32_t>(entry.page->ownershipType);
            currentSection.perms = entry.page->flags;
            strncpy(currentSection.path, entry.page->filename.c_str(), sizeof(currentSection.path) - 1);
            currentSection.path[sizeof(currentSection.path) - 1] = '\0';
        }
    }

    // Save final section
    sections.push_back(currentSection);

    return !sections.empty();
}

// Check if a PID has been scanned
bool PageDatabase::IsPIDScanned(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex);
    return scannedPIDs.find(pid) != scannedPIDs.end();
}

// Query methods
bool PageDatabase::IsScanning() const {
    return scanner && scanner->IsScanning();
}

bool PageDatabase::IsFullScanComplete() const {
    return fullScanComplete.load();
}

size_t PageDatabase::GetScannedProcessCount() const {
    return scannedProcessCount.load();
}

size_t PageDatabase::GetTotalProcessCount() const {
    return totalProcessCount.load();
}

size_t PageDatabase::GetScanGeneration() const {
    return scanGeneration.load();
}

void PageDatabase::StartBackgroundScanning(KernelDiscoveryBackend* backend, int rescanIntervalSec) {
    StopBackgroundScanning();

    // Reset scan state
    fullScanComplete.store(false);
    scannedProcessCount.store(0);
    totalProcessCount.store(0);

    {
        std::lock_guard<std::mutex> lock(mutex);
        scannedPIDs.clear();
    }

    scanner = std::make_unique<BackgroundScanner>(this, backend, rescanIntervalSec);
    scanner->Start();
}

void PageDatabase::StopBackgroundScanning() {
    if (scanner) {
        scanner->Stop();
        scanner.reset();
    }
}

std::string PageDatabase::GetProcessName(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = processNames.find(pid);
    if (it != processNames.end()) {
        return it->second;
    }
    return "";
}

std::unordered_map<uint32_t, std::string> PageDatabase::GetAllProcessNames() const {
    std::lock_guard<std::mutex> lock(mutex);
    return processNames;
}

} // namespace Haywire
