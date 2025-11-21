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

// Include IKernelDiscovery interface
#include "ikernel_discovery.h"

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

    // Initialize with memory file path, QMP settings, and guest OS hint
    bool Initialize(const std::string& memoryPath = "/tmp/haywire-vm-mem",
                   const std::string& qmpHost = "localhost",
                   int qmpPort = 4445,
                   GuestOS guestOS = GuestOS::Unknown);
    void Cleanup();

    // Control debug logging
    void SetDebugLogging(bool enabled);

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
    IKernelDiscovery* GetDiscovery() const { return discovery.get(); }

    // Get current process info (from IKernelDiscovery::ProcessInfo)
    const IKernelDiscovery::ProcessInfo* GetCurrentProcess() const;

private:
    std::unique_ptr<IKernelDiscovery> discovery;
    std::unordered_map<uint32_t, const IKernelDiscovery::ProcessInfo*> pidMap;
    uint32_t currentPID;
    bool initialized;
    std::chrono::steady_clock::time_point lastRefreshTime;
};

} // namespace Haywire