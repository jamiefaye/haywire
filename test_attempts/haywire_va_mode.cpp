/**
 * Haywire VA Mode Integration
 *
 * This shows how to integrate kernel discovery into Haywire's VA mode
 * to automatically discover processes and translate virtual addresses
 */

#include "kernel_discovery.cpp"
#include <memory>
#include <unordered_map>

// Type aliases for convenience
using ProcessInfo = Haywire::KernelDiscovery::ProcessInfo;
using MemorySection = Haywire::KernelDiscovery::MemorySection;
using PTE = Haywire::KernelDiscovery::PTE;

class HaywireVAMode {
private:
    std::unique_ptr<Haywire::KernelDiscovery> discovery;
    std::unordered_map<uint32_t, const ProcessInfo*> pidMap;
    uint32_t currentPID = 0;
    bool initialized = false;

public:
    HaywireVAMode() : discovery(std::make_unique<Haywire::KernelDiscovery>()) {}

    // Initialize VA mode - called when user enables VA mode
    bool Initialize() {
        std::cout << "Initializing VA mode with kernel discovery..." << std::endl;

        // Initialize kernel discovery
        if (!discovery->Initialize()) {
            std::cerr << "Failed to initialize kernel discovery" << std::endl;
            return false;
        }

        // Discover kernel structures
        if (!discovery->DiscoverKernel()) {
            std::cerr << "Failed to discover kernel" << std::endl;
            return false;
        }

        // Find all processes
        if (!discovery->DiscoverProcesses()) {
            std::cerr << "Failed to discover processes" << std::endl;
            return false;
        }

        // Extract PGDs, memory sections, and PTEs
        discovery->ExtractProcessPGDs();

        // Build PID map for quick lookup
        const auto& processes = discovery->GetProcesses();
        for (const auto& proc : processes) {
            if (!proc.is_kernel_thread) {
                pidMap[proc.pid] = &proc;
            }
        }

        std::cout << "VA mode initialized:" << std::endl;
        std::cout << "  Found " << pidMap.size() << " user processes" << std::endl;

        // Show available processes
        ShowProcessList();

        initialized = true;
        return true;
    }

    // Show list of available processes
    void ShowProcessList() {
        std::cout << "\nAvailable processes for VA mode:" << std::endl;
        std::cout << "PID     | Name             | Memory  | Sections | PTEs" << std::endl;
        std::cout << "--------|------------------|---------|----------|-------" << std::endl;

        for (const auto& [pid, proc] : pidMap) {
            uint64_t totalMemory = 0;
            for (const auto& pte : proc->ptes) {
                totalMemory += pte.size;
            }

            std::cout << std::setw(7) << pid << " | "
                      << std::setw(16) << std::left << proc->comm << " | "
                      << std::setw(5) << std::right << (totalMemory / (1024*1024)) << " MB | "
                      << std::setw(8) << proc->sections.size() << " | "
                      << std::setw(5) << proc->ptes.size() << std::endl;
        }
    }

    // Select a process for VA translation
    bool SelectProcess(uint32_t pid) {
        auto it = pidMap.find(pid);
        if (it == pidMap.end()) {
            std::cerr << "Process PID " << pid << " not found" << std::endl;
            return false;
        }

        currentPID = pid;
        const auto* proc = it->second;

        std::cout << "\nSelected process: PID " << pid << " (" << proc->comm << ")" << std::endl;
        std::cout << "  PGD: 0x" << std::hex << proc->pgd << std::dec << std::endl;
        std::cout << "  Memory sections: " << proc->sections.size() << std::endl;
        std::cout << "  Mapped pages: " << proc->ptes.size() << std::endl;

        // Show memory layout
        if (!proc->sections.empty()) {
            std::cout << "\nMemory layout:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(10), proc->sections.size()); i++) {
                const auto& sec = proc->sections[i];
                std::cout << "  0x" << std::hex << sec.start
                          << "-0x" << sec.end << std::dec
                          << " (" << ((sec.end - sec.start) / 1024) << " KB)";
                if (!sec.name.empty()) {
                    std::cout << " [" << sec.name << "]";
                }
                std::cout << std::endl;
            }
            if (proc->sections.size() > 10) {
                std::cout << "  ... and " << (proc->sections.size() - 10) << " more sections" << std::endl;
            }
        }

        return true;
    }

    // Translate a virtual address to physical address for current process
    uint64_t TranslateVA(uint64_t va) {
        if (!initialized || currentPID == 0) {
            std::cerr << "VA mode not initialized or no process selected" << std::endl;
            return 0;
        }

        auto it = pidMap.find(currentPID);
        if (it == pidMap.end()) {
            return 0;
        }

        const auto* proc = it->second;

        // First check if it's in our PTE cache
        for (const auto& pte : proc->ptes) {
            if (va >= pte.va && va < pte.va + pte.size) {
                uint64_t offset = va - pte.va;
                uint64_t pa = pte.pa + offset;
                return pa;
            }
        }

        // Not in PTE cache - could walk page tables directly if needed
        std::cerr << "VA 0x" << std::hex << va << std::dec << " not found in PTE cache" << std::endl;
        return 0;
    }

    // Get memory section info for a virtual address
    const MemorySection* GetSectionForVA(uint64_t va) {
        if (!initialized || currentPID == 0) {
            return nullptr;
        }

        auto it = pidMap.find(currentPID);
        if (it == pidMap.end()) {
            return nullptr;
        }

        const auto* proc = it->second;
        for (const auto& sec : proc->sections) {
            if (va >= sec.start && va < sec.end) {
                return &sec;
            }
        }

        return nullptr;
    }

    // Read memory at virtual address
    bool ReadVirtualMemory(uint64_t va, void* buffer, size_t size) {
        uint64_t pa = TranslateVA(va);
        if (pa == 0) {
            return false;
        }

        // Here you would read from the physical address
        // For example, using the memory-mapped file:
        // memcpy(buffer, memBase + (pa - GUEST_RAM_START), size);

        std::cout << "Would read " << size << " bytes from VA 0x"
                  << std::hex << va << " (PA 0x" << pa << ")" << std::dec << std::endl;
        return true;
    }

    // Get current process info
    const ProcessInfo* GetCurrentProcess() {
        if (currentPID == 0) return nullptr;
        auto it = pidMap.find(currentPID);
        return (it != pidMap.end()) ? it->second : nullptr;
    }

    // Refresh process list (re-discover)
    void Refresh() {
        std::cout << "Refreshing process list..." << std::endl;

        pidMap.clear();
        currentPID = 0;

        discovery->DiscoverProcesses();
        discovery->ExtractProcessPGDs();

        const auto& processes = discovery->GetProcesses();
        for (const auto& proc : processes) {
            if (!proc.is_kernel_thread) {
                pidMap[proc.pid] = &proc;
            }
        }

        std::cout << "Found " << pidMap.size() << " user processes" << std::endl;
    }
};

// Example usage - this would be integrated into Haywire's main loop
int main() {
    HaywireVAMode vaMode;

    // Initialize VA mode
    if (!vaMode.Initialize()) {
        std::cerr << "Failed to initialize VA mode" << std::endl;
        return 1;
    }

    // Select a process (e.g., bash)
    for (const auto* proc = vaMode.GetCurrentProcess(); proc == nullptr; ) {
        std::cout << "\nEnter PID to select (or 0 to refresh): ";
        uint32_t pid;
        std::cin >> pid;

        if (pid == 0) {
            vaMode.Refresh();
            vaMode.ShowProcessList();
        } else {
            vaMode.SelectProcess(pid);
            proc = vaMode.GetCurrentProcess();
        }
    }

    // Example: translate some addresses
    const auto* proc = vaMode.GetCurrentProcess();
    if (proc && !proc->sections.empty()) {
        // Try to translate the first section's start address
        uint64_t testVA = proc->sections[0].start;
        uint64_t pa = vaMode.TranslateVA(testVA);

        if (pa != 0) {
            std::cout << "\nTranslation test:" << std::endl;
            std::cout << "  VA 0x" << std::hex << testVA
                      << " -> PA 0x" << pa << std::dec << std::endl;

            const auto* section = vaMode.GetSectionForVA(testVA);
            if (section && !section->name.empty()) {
                std::cout << "  Section: " << section->name << std::endl;
            }
        }
    }

    std::cout << "\nVA mode ready for use!" << std::endl;
    std::cout << "You can now browse memory using virtual addresses" << std::endl;

    return 0;
}