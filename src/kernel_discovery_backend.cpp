/**
 * Kernel Discovery Backend for Haywire
 *
 * Replaces beacon/companion system with host-side kernel discovery
 * using QMP and memory scanning
 */

#include "kernel_discovery_backend.h"
#include "memory_types.h"  // For SectionEntry definition
#include "../include/ikernel_discovery.h"  // For CreateKernelDiscovery factory
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cstring>  // For memset and strncpy

namespace Haywire {

KernelDiscoveryBackend::KernelDiscoveryBackend()
    : initialized(false), currentPID(0) {
    // Don't create discovery here - will be created in Initialize()
}

KernelDiscoveryBackend::~KernelDiscoveryBackend() {
    Cleanup();
}

bool KernelDiscoveryBackend::Initialize(const std::string& memoryPath,
                                        const std::string& qmpHost,
                                        int qmpPort,
                                        GuestOS guestOS) {
    if (initialized) {
        return true;
    }

    std::cout << "[KernelDiscovery] Initializing with memory file: " << memoryPath << std::endl;
    std::cout << "[KernelDiscovery] QMP connection: " << qmpHost << ":" << qmpPort << std::endl;
    std::cout << "[KernelDiscovery] Guest OS: " << (guestOS == GuestOS::Windows ? "Windows" : guestOS == GuestOS::Linux ? "Linux" : "Auto-detect") << std::endl;

    // Create kernel discovery using factory (supports Linux and Windows)
    discovery = CreateKernelDiscovery(memoryPath, "", guestOS);
    if (!discovery) {
        std::cerr << "[KernelDiscovery] Failed to create kernel discovery backend" << std::endl;
        return false;
    }

    std::cout << "[KernelDiscovery] Calling Initialize()..." << std::endl;
    if (!discovery->Initialize()) {
        std::cerr << "[KernelDiscovery] Failed to initialize kernel discovery" << std::endl;
        return false;
    }

    // Discover kernel structures (gets swapper PGD from QMP)
    std::cout << "[KernelDiscovery] Calling DiscoverKernel() to find swapper PGD..." << std::endl;
    if (!discovery->DiscoverKernel()) {
        std::cerr << "[KernelDiscovery] Failed to discover kernel structures - cannot proceed without QMP" << std::endl;
        return false;  // Fail initialization if we can't get swapper PGD
    }

    // Initial process discovery
    std::cout << "[KernelDiscovery] Starting process discovery..." << std::endl;
    if (!RefreshProcessList()) {
        std::cerr << "[KernelDiscovery] Failed to discover processes" << std::endl;
        return false;
    }

    initialized = true;
    std::cout << "[KernelDiscovery] Initialized successfully" << std::endl;
    return true;
}

void KernelDiscoveryBackend::Cleanup() {
    pidMap.clear();
    currentPID = 0;
    initialized = false;
}

bool KernelDiscoveryBackend::RefreshProcessList() {
    auto startTime = std::chrono::steady_clock::now();

    // Discover processes
    std::cout << "[KernelDiscovery] Scanning memory for processes..." << std::endl;
    if (!discovery->DiscoverProcesses()) {
        std::cerr << "[KernelDiscovery] DiscoverProcesses() failed" << std::endl;
        return false;
    }

    // Extract PGDs, memory sections, and PTEs
    std::cout << "[KernelDiscovery] Extracting PGDs and memory sections..." << std::endl;
    discovery->ExtractProcessPGDs();

    // Build PID map for quick lookup
    pidMap.clear();
    const auto& processes = discovery->GetProcesses();

    for (const auto& proc : processes) {
        if (!proc.is_kernel_thread) {
            pidMap[proc.pid] = &proc;
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "[KernelDiscovery] Found " << pidMap.size() << " user processes in "
              << duration << "ms" << std::endl;

    lastRefreshTime = std::chrono::steady_clock::now();
    return true;
}

bool KernelDiscoveryBackend::GetPIDList(std::vector<uint32_t>& pids) {
    pids.clear();

    for (const auto& [pid, proc] : pidMap) {
        pids.push_back(pid);
    }

    std::sort(pids.begin(), pids.end());
    return !pids.empty();
}

bool KernelDiscoveryBackend::GetProcessInfo(uint32_t pid, ProcessInfo& info) {
    auto it = pidMap.find(pid);
    if (it == pidMap.end()) {
        return false;
    }

    const auto* proc = it->second;

    info.pid = proc->pid;
    info.name = proc->comm;

    // Heuristic for process state based on process characteristics
    // TODO: Extract actual state from task_struct at offset 0x18
    if (proc->comm.find("kworker") != std::string::npos ||
        proc->comm.find("ksoftirqd") != std::string::npos ||
        proc->comm.find("migration") != std::string::npos ||
        proc->comm.find("kthread") != std::string::npos ||
        proc->comm.find("kcompactd") != std::string::npos ||
        proc->comm.find("khugepaged") != std::string::npos ||
        proc->comm.find("kswapd") != std::string::npos ||
        proc->comm[0] == '[') {  // Kernel threads often have [brackets]
        info.state = 'S';  // Most kernel threads are sleeping
    } else if (proc->pid == 1) {
        info.state = 'S';  // systemd/init is usually sleeping
    } else if (proc->comm == "systemd" || proc->comm == "sshd" ||
               proc->comm == "snapd" || proc->comm == "networkd") {
        info.state = 'S';  // System daemons usually sleeping
    } else if (proc->comm == "bash" || proc->comm == "sh") {
        info.state = 'S';  // Shells are usually sleeping waiting for input
    } else {
        // Default to sleeping for most processes - more realistic than all running
        info.state = 'S';
    }

    info.pgd = proc->pgd;
    info.hasDetails = true;

    // Calculate memory usage from PTEs
    info.vsize = 0;
    info.rss = 0;
    for (const auto& pte : proc->ptes) {
        info.vsize += pte.size;
        if (pte.present) {
            info.rss += pte.size;
        }
    }

    // Count threads (processes with same mm_struct)
    info.num_threads = 1;
    for (const auto& [otherpid, otherproc] : pidMap) {
        if (otherpid != pid && otherproc->mm_addr == proc->mm_addr) {
            info.num_threads++;
        }
    }

    return true;
}

std::map<uint32_t, ProcessInfo> KernelDiscoveryBackend::GetAllProcessInfo() {
    std::map<uint32_t, ProcessInfo> result;

    for (const auto& [pid, proc] : pidMap) {
        ProcessInfo info;
        if (GetProcessInfo(pid, info)) {
            result[pid] = info;
        }
    }

    return result;
}

bool KernelDiscoveryBackend::DiscoverProcesses(std::vector<ProcessInfo>& processes) {
    processes.clear();

    for (const auto& [pid, proc] : pidMap) {
        ProcessInfo info;
        if (GetProcessInfo(pid, info)) {
            processes.push_back(info);
        }
    }

    return !processes.empty();
}

bool KernelDiscoveryBackend::SelectProcess(uint32_t pid) {
    auto it = pidMap.find(pid);
    if (it == pidMap.end()) {
        std::cerr << "[KernelDiscovery] Process PID " << pid << " not found" << std::endl;
        return false;
    }

    currentPID = pid;
    const auto* proc = it->second;

    std::cout << "[KernelDiscovery] Selected process: PID " << pid
              << " (" << proc->comm << ")" << std::endl;
    std::cout << "  PGD: 0x" << std::hex << proc->pgd << std::dec << std::endl;
    std::cout << "  Memory sections: " << proc->sections.size() << std::endl;
    std::cout << "  Mapped pages: " << proc->ptes.size() << std::endl;

    return true;
}

uint64_t KernelDiscoveryBackend::TranslateVA(uint64_t va) {
    if (!initialized || currentPID == 0) {
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

    // Not found in PTE cache
    return 0;
}

bool KernelDiscoveryBackend::GetProcessSections(uint32_t pid, std::vector<SectionEntry>& sections) {
    sections.clear();

    auto it = pidMap.find(pid);
    if (it == pidMap.end()) {
        return false;
    }

    const auto* proc = it->second;

    // If sections haven't been extracted yet, do it now
    if (proc->sections.empty()) {
        discovery->ExtractProcessMemoryMap(pid);
        // Note: proc pointer may be invalidated after ExtractProcessMemoryMap
        // Re-fetch from map
        it = pidMap.find(pid);
        if (it == pidMap.end()) {
            return false;
        }
        proc = it->second;

        // Also extract PTEs if not done yet
        if (proc->ptes.empty()) {
            discovery->WalkProcessPageTables(const_cast<IKernelDiscovery::ProcessInfo&>(*proc));
            // Re-fetch again in case PTEs modified the process
            it = pidMap.find(pid);
            if (it == pidMap.end()) {
                return false;
            }
            proc = it->second;
        }
    }

    for (const auto& sec : proc->sections) {
        SectionEntry entry;
        entry.type = 2;  // ENTRY_SECTION
        entry.pid = pid;
        entry.va_start = sec.start;
        entry.va_end = sec.end;

        // Convert flags to permissions
        uint32_t perms = 0;
        if (sec.flags & 0x1) perms |= 4;  // VM_READ -> readable
        if (sec.flags & 0x2) perms |= 2;  // VM_WRITE -> writable
        if (sec.flags & 0x4) perms |= 1;  // VM_EXEC -> executable
        entry.perms = perms;

        // Copy ownership type (enum values match between MemorySection::Type and our ownership_type field)
        entry.ownership_type = static_cast<uint32_t>(sec.type);

        // Copy name to path field (truncate if too long)
        std::memset(entry.path, 0, sizeof(entry.path));
        if (!sec.name.empty()) {
            std::strncpy(entry.path, sec.name.c_str(), sizeof(entry.path) - 1);
        }

        sections.push_back(entry);
    }

    return !sections.empty();
}

bool KernelDiscoveryBackend::GetProcessPTEs(uint32_t pid, std::unordered_map<uint64_t, uint64_t>& ptes) {
    ptes.clear();

    auto it = pidMap.find(pid);
    if (it == pidMap.end()) {
        return false;
    }

    const auto* proc = it->second;

    // Build PTE map (VA -> PA)
    for (const auto& pte : proc->ptes) {
        // For huge pages, we need to map all 4KB pages within
        if (pte.size > 0x1000) {
            for (uint64_t offset = 0; offset < pte.size; offset += 0x1000) {
                ptes[pte.va + offset] = pte.pa + offset;
            }
        } else {
            ptes[pte.va] = pte.pa;
        }
    }

    return !ptes.empty();
}

bool KernelDiscoveryBackend::ShouldRefresh() {
    if (!initialized) {
        return false;
    }

    // Check if it's been more than 5 seconds since last refresh
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastRefreshTime).count();

    return elapsed >= 5;
}

const IKernelDiscovery::ProcessInfo* KernelDiscoveryBackend::GetCurrentProcess() const {
    if (currentPID == 0) return nullptr;

    auto it = pidMap.find(currentPID);
    return (it != pidMap.end()) ? it->second : nullptr;
}

} // namespace Haywire