#pragma once

/**
 * Kernel Discovery Backend for Haywire
 *
 * Replaces beacon/companion system with host-side kernel discovery
 */

#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <chrono>

// Forward declare to avoid including the whole kernel_discovery.cpp
namespace Haywire {
    class KernelDiscovery;
}

// Include our kernel discovery implementation
#include "../src/kernel_discovery.cpp"

namespace Haywire {

// Process information compatible with BeaconReader interface
struct ProcessInfo {
    uint32_t pid;
    uint32_t ppid = 0;  // TODO: Extract from task_struct
    std::string name;
    char state;
    uint64_t vsize;
    uint64_t rss;
    uint32_t num_threads;
    std::string exe_path;
    bool hasDetails;
    uint64_t pgd;  // Physical address of page global directory
};

// Use existing SectionEntry from beacon_decoder.h
// Forward declaration to avoid include dependency
struct SectionEntry;

class KernelDiscoveryBackend {
public:
    KernelDiscoveryBackend();
    ~KernelDiscoveryBackend();

    // Initialize with memory file path and QMP settings
    bool Initialize(const std::string& memoryPath = "/tmp/haywire-vm-mem",
                   const std::string& qmpHost = "localhost",
                   int qmpPort = 4445);
    void Cleanup();

    // Process discovery
    bool RefreshProcessList();
    bool GetPIDList(std::vector<uint32_t>& pids);

    // Process information
    bool GetProcessInfo(uint32_t pid, ProcessInfo& info);
    std::map<uint32_t, ProcessInfo> GetAllProcessInfo();

    // Process selection for VA mode
    bool SelectProcess(uint32_t pid);
    uint32_t GetCurrentPID() const { return currentPID; }

    // Virtual address translation
    uint64_t TranslateVA(uint64_t va);

    // Get process memory sections (VMAs)
    bool GetProcessSections(uint32_t pid, std::vector<SectionEntry>& sections);

    // Get memory sections for ALL processes (for PageDatabase)
    bool GetProcessMemorySections(uint32_t pid, std::vector<SectionEntry>& sections) {
        return GetProcessSections(pid, sections);
    }

    // Discover all processes (for PageDatabase)
    bool DiscoverProcesses(std::vector<ProcessInfo>& processes);

    // Get process PTEs for crunched view
    bool GetProcessPTEs(uint32_t pid, std::unordered_map<uint64_t, uint64_t>& ptes);

    // Check if we should refresh (for auto-refresh)
    bool ShouldRefresh();

    // Get direct access to kernel discovery
    KernelDiscovery* GetDiscovery() const { return discovery.get(); }

    // Get current process info
    const KernelDiscovery::ProcessInfo* GetCurrentProcess() const;

private:
    std::unique_ptr<KernelDiscovery> discovery;
    std::unordered_map<uint32_t, const KernelDiscovery::ProcessInfo*> pidMap;
    uint32_t currentPID;
    bool initialized;
    std::chrono::steady_clock::time_point lastRefreshTime;
};

} // namespace Haywire