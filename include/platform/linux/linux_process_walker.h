#pragma once

#include "platform/process_walker.h"
#include <map>

namespace Haywire {

// Linux kernel offsets for different architectures and versions
struct LinuxKernelOffsets {
    // task_struct offsets
    size_t pid;           // PID field
    size_t comm;          // Command name
    size_t tasks_next;    // Next task in list
    size_t tasks_prev;    // Previous task in list
    size_t mm;            // mm_struct pointer
    size_t parent;        // Parent task_struct pointer
    size_t thread_group;  // Thread group list

    // mm_struct offsets
    size_t mm_pgd;        // Page directory pointer (CR3/TTBR0)
    size_t mm_start_code; // Start of code segment
    size_t mm_end_code;   // End of code segment
    size_t mm_start_data; // Start of data segment
    size_t mm_end_data;   // End of data segment
};

// Linux-specific process walker
class LinuxProcessWalker : public ProcessWalker {
public:
    LinuxProcessWalker(MemoryBackend* backend);
    ~LinuxProcessWalker() override;

    // Initialize the walker (find init_task)
    bool Initialize() override;

    // Walk the process list and return all processes
    std::vector<ProcessInfo> EnumerateProcesses() override;

    // Find a specific process by PID
    bool FindProcess(uint64_t pid, ProcessInfo& info) override;

    // Find processes by name (partial match)
    std::vector<ProcessInfo> FindProcessesByName(const std::string& name) override;

    // Get OS name
    const char* GetOSName() const override { return "Linux"; }

    // Get kernel version if available
    std::string GetKernelVersion() const override;

    // Linux-specific: Set custom kernel offsets
    void SetKernelOffsets(const LinuxKernelOffsets& offsets);

    // Linux-specific: Try to auto-detect kernel offsets
    bool AutoDetectOffsets();

    // Linux-specific: Get init_task address
    uint64_t GetInitTaskAddress() const { return initTaskAddr; }

private:
    uint64_t initTaskAddr;
    uint64_t swapperTaskAddr;  // PID 0 task
    LinuxKernelOffsets offsets;
    std::string kernelVersion;
    bool offsetsDetected;

    // Common offset configurations for different kernel versions
    static const std::vector<LinuxKernelOffsets> KNOWN_OFFSET_CONFIGS;

    // Helper to read task_struct fields
    bool ReadTaskStruct(uint64_t taskAddr, ProcessInfo& info);

    // Helper to validate a potential task_struct
    bool ValidateTaskStruct(uint64_t addr);

    // Helper to find init_task through various methods
    bool FindInitTask();

    // Helper to walk the task list
    std::vector<ProcessInfo> WalkTaskList(uint64_t startTask);

    // Architecture-specific offset adjustments
    void AdjustOffsetsForArchitecture(const std::string& arch);
};

}