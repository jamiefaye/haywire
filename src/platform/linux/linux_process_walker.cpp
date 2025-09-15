#include "platform/linux/linux_process_walker.h"
#include "memory_backend.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <set>

namespace Haywire {

// Common offset configurations for different kernel versions
const std::vector<LinuxKernelOffsets> LinuxProcessWalker::KNOWN_OFFSET_CONFIGS = {
    // Linux 5.15+ common layout (ARM64)
    {0x4E8, 0x738, 0x3A0, 0x3A8, 0x520, 0x2E8, 0x320, 0x48, 0x80, 0x88, 0x90, 0x98},
    {0x4E0, 0x730, 0x398, 0x3A0, 0x518, 0x2E0, 0x318, 0x48, 0x80, 0x88, 0x90, 0x98},
    // Linux 5.10 (ARM64)
    {0x398, 0x5C8, 0x2E0, 0x2E8, 0x3F0, 0x250, 0x280, 0x48, 0x80, 0x88, 0x90, 0x98},
    // Linux 5.4 (ARM64)
    {0x3A0, 0x5D0, 0x2E8, 0x2F0, 0x3F8, 0x258, 0x288, 0x48, 0x80, 0x88, 0x90, 0x98},
    // x86-64 common offsets (slightly different)
    {0x398, 0x5E0, 0x2F0, 0x2F8, 0x400, 0x260, 0x290, 0x50, 0x88, 0x90, 0x98, 0xA0},
};

LinuxProcessWalker::LinuxProcessWalker(MemoryBackend* backend)
    : ProcessWalker(backend), initTaskAddr(0), swapperTaskAddr(0),
      offsetsDetected(false) {
    // Start with the first known configuration
    if (!KNOWN_OFFSET_CONFIGS.empty()) {
        offsets = KNOWN_OFFSET_CONFIGS[0];
    }
}

LinuxProcessWalker::~LinuxProcessWalker() {
}

bool LinuxProcessWalker::Initialize() {
    std::cout << "Initializing Linux process walker...\n";

    // Try to auto-detect offsets first
    if (!offsetsDetected) {
        if (AutoDetectOffsets()) {
            std::cout << "Successfully auto-detected kernel offsets\n";
        } else {
            std::cout << "Using default kernel offsets\n";
        }
    }

    // Find init_task
    if (!FindInitTask()) {
        std::cerr << "Failed to find init_task\n";
        return false;
    }

    std::cout << "Found init_task at 0x" << std::hex << initTaskAddr << std::dec << "\n";
    return true;
}

std::vector<ProcessInfo> LinuxProcessWalker::EnumerateProcesses() {
    if (initTaskAddr == 0) {
        std::cerr << "Process walker not initialized\n";
        return {};
    }

    return WalkTaskList(initTaskAddr);
}

bool LinuxProcessWalker::FindProcess(uint64_t pid, ProcessInfo& info) {
    auto processes = EnumerateProcesses();
    for (const auto& proc : processes) {
        if (proc.pid == pid) {
            info = proc;
            return true;
        }
    }
    return false;
}

std::vector<ProcessInfo> LinuxProcessWalker::FindProcessesByName(const std::string& name) {
    std::vector<ProcessInfo> matches;
    auto processes = EnumerateProcesses();

    for (const auto& proc : processes) {
        if (proc.name.find(name) != std::string::npos) {
            matches.push_back(proc);
        }
    }

    return matches;
}

std::string LinuxProcessWalker::GetKernelVersion() const {
    if (!kernelVersion.empty()) {
        return kernelVersion;
    }

    // Try to read version from known location
    // This would need QMP or other method to get kernel version
    return "Linux (version unknown)";
}

void LinuxProcessWalker::SetKernelOffsets(const LinuxKernelOffsets& newOffsets) {
    offsets = newOffsets;
    offsetsDetected = true;
}

bool LinuxProcessWalker::AutoDetectOffsets() {
    std::cout << "Attempting to auto-detect kernel offsets...\n";

    // Try each known configuration
    for (const auto& config : KNOWN_OFFSET_CONFIGS) {
        offsets = config;

        // Try to find and validate init_task with these offsets
        if (FindInitTask() && ValidateTaskStruct(initTaskAddr)) {
            offsetsDetected = true;
            std::cout << "Detected offsets: PID=" << std::hex << offsets.pid
                     << " COMM=" << offsets.comm << std::dec << "\n";
            return true;
        }
    }

    return false;
}

bool LinuxProcessWalker::ReadTaskStruct(uint64_t taskAddr, ProcessInfo& info) {
    if (taskAddr == 0) {
        return false;
    }

    info.task_struct_addr = taskAddr;

    // Read PID
    std::vector<uint8_t> pidBuf;
    if (memory->Read(taskAddr + offsets.pid, 4, pidBuf) && pidBuf.size() == 4) {
        memcpy(&info.pid, pidBuf.data(), 4);
    }

    // Read command name (16 bytes)
    std::vector<uint8_t> commBuf;
    if (memory->Read(taskAddr + offsets.comm, 16, commBuf) && commBuf.size() == 16) {
        // Null-terminate and copy
        char comm[17] = {0};
        memcpy(comm, commBuf.data(), 16);
        info.name = comm;
    }

    // Read mm_struct pointer
    std::vector<uint8_t> mmBuf;
    if (memory->Read(taskAddr + offsets.mm, 8, mmBuf) && mmBuf.size() == 8) {
        memcpy(&info.mm_struct_addr, mmBuf.data(), 8);

        // If we have mm_struct, read the page table base (pgd)
        if (info.mm_struct_addr != 0) {
            std::vector<uint8_t> pgdBuf;
            if (memory->Read(info.mm_struct_addr + offsets.mm_pgd, 8, pgdBuf) && pgdBuf.size() == 8) {
                memcpy(&info.page_table_base, pgdBuf.data(), 8);
            }
        }
    }

    // Read parent pointer
    std::vector<uint8_t> parentBuf;
    if (memory->Read(taskAddr + offsets.parent, 8, parentBuf) && parentBuf.size() == 8) {
        uint64_t parentAddr;
        memcpy(&parentAddr, parentBuf.data(), 8);

        // Read parent PID
        if (parentAddr != 0) {
            std::vector<uint8_t> parentPidBuf;
            if (memory->Read(parentAddr + offsets.pid, 4, parentPidBuf) && parentPidBuf.size() == 4) {
                memcpy(&info.parent_pid, parentPidBuf.data(), 4);
            }
        }
    }

    return true;
}

bool LinuxProcessWalker::ValidateTaskStruct(uint64_t addr) {
    if (addr == 0 || addr < 0xFFFF000000000000ULL) {
        return false;  // Not a kernel address
    }

    // Read potential PID
    std::vector<uint8_t> pidBuf;
    if (!memory->Read(addr + offsets.pid, 4, pidBuf) || pidBuf.size() != 4) {
        return false;
    }

    uint32_t pid;
    memcpy(&pid, pidBuf.data(), 4);

    // PID should be reasonable (0-65535)
    if (pid > 65535) {
        return false;
    }

    // Read potential comm field
    std::vector<uint8_t> commBuf;
    if (!memory->Read(addr + offsets.comm, 16, commBuf) || commBuf.size() != 16) {
        return false;
    }

    // Check if comm looks like a valid string
    bool hasNull = false;
    bool hasValidChars = false;
    for (size_t i = 0; i < 16; i++) {
        if (commBuf[i] == 0) {
            hasNull = true;
        } else if (commBuf[i] >= 32 && commBuf[i] < 127) {
            hasValidChars = true;
        } else if (commBuf[i] != 0 && (commBuf[i] < 32 || commBuf[i] >= 127)) {
            return false;  // Invalid character
        }
    }

    return hasValidChars;  // Should have at least some valid characters
}

bool LinuxProcessWalker::FindInitTask() {
    // Try multiple methods to find init_task

    // Method 1: Look for swapper/init in known memory regions
    // This would typically use QMP to get current task pointer
    // For now, we'll use a simplified approach

    // Common init_task addresses on ARM64
    std::vector<uint64_t> commonAddresses = {
        0xFFFF800011C10000ULL,  // Common on some kernels
        0xFFFF800011A10000ULL,
        0xFFFF800011810000ULL,
        0xFFFF800010000000ULL + 0x1C10000,  // Offset from kernel base
    };

    for (uint64_t addr : commonAddresses) {
        if (ValidateTaskStruct(addr)) {
            ProcessInfo info;
            if (ReadTaskStruct(addr, info) && info.pid == 0) {
                swapperTaskAddr = addr;

                // Follow the tasks list to find PID 1 (init)
                std::vector<uint8_t> nextBuf;
                if (memory->Read(addr + offsets.tasks_next, 8, nextBuf) && nextBuf.size() == 8) {
                    uint64_t nextAddr;
                    memcpy(&nextAddr, nextBuf.data(), 8);

                    // Adjust for list_head offset
                    uint64_t taskAddr = nextAddr - offsets.tasks_next;
                    if (ValidateTaskStruct(taskAddr)) {
                        if (ReadTaskStruct(taskAddr, info) && info.pid == 1) {
                            initTaskAddr = taskAddr;
                            return true;
                        }
                    }
                }

                // If we found swapper but not init, use swapper
                initTaskAddr = swapperTaskAddr;
                return true;
            }
        }
    }

    return false;
}

std::vector<ProcessInfo> LinuxProcessWalker::WalkTaskList(uint64_t startTask) {
    std::vector<ProcessInfo> processes;
    std::set<uint64_t> visited;  // Prevent infinite loops

    uint64_t currentTask = startTask;
    const size_t MAX_PROCESSES = 10000;

    while (processes.size() < MAX_PROCESSES) {
        // Check if we've already visited this task
        if (visited.count(currentTask) > 0) {
            break;  // Loop detected
        }
        visited.insert(currentTask);

        // Read this task's info
        ProcessInfo info;
        if (ReadTaskStruct(currentTask, info)) {
            processes.push_back(info);
        }

        // Get next task in the list
        std::vector<uint8_t> nextBuf;
        if (!memory->Read(currentTask + offsets.tasks_next, 8, nextBuf) || nextBuf.size() != 8) {
            break;
        }

        uint64_t nextPtr;
        memcpy(&nextPtr, nextBuf.data(), 8);

        // Adjust for list_head offset to get actual task_struct address
        uint64_t nextTask = nextPtr - offsets.tasks_next;

        // Check if we've looped back to the start
        if (nextTask == startTask) {
            break;
        }

        // Validate the next task before continuing
        if (!ValidateTaskStruct(nextTask)) {
            break;
        }

        currentTask = nextTask;
    }

    std::cout << "Found " << processes.size() << " processes\n";
    return processes;
}

void LinuxProcessWalker::AdjustOffsetsForArchitecture(const std::string& arch) {
    // Adjust offsets based on architecture if needed
    // x86-64 and ARM64 have slightly different layouts
    if (arch == "x86_64" || arch == "x64") {
        // x86-64 specific adjustments
        // These would be refined based on actual kernel builds
    } else if (arch == "arm64" || arch == "aarch64") {
        // ARM64 specific adjustments (current defaults)
    }
}

}