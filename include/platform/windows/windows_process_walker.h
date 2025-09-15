#pragma once

#include "platform/process_walker.h"

namespace Haywire {

// Windows kernel offsets for EPROCESS structure
struct WindowsKernelOffsets {
    // EPROCESS offsets
    size_t UniqueProcessId;     // PID
    size_t ImageFileName;        // Process name (15 chars max)
    size_t ActiveProcessLinks;   // LIST_ENTRY for process list
    size_t DirectoryTableBase;   // CR3 value
    size_t Peb;                  // PEB address in user space
    size_t InheritedFromUniqueProcessId; // Parent PID
    size_t ThreadListHead;       // Thread list

    // PEB offsets
    size_t ProcessParameters;    // RTL_USER_PROCESS_PARAMETERS
    size_t ImageBaseAddress;     // Main module base
    size_t Ldr;                  // PEB_LDR_DATA

    // KTHREAD offsets
    size_t Process;              // Owning EPROCESS
};

// Windows-specific process walker
class WindowsProcessWalker : public ProcessWalker {
public:
    WindowsProcessWalker(MemoryBackend* backend);
    ~WindowsProcessWalker() override;

    // Initialize the walker (find PsInitialSystemProcess)
    bool Initialize() override;

    // Walk the process list and return all processes
    std::vector<ProcessInfo> EnumerateProcesses() override;

    // Find a specific process by PID
    bool FindProcess(uint64_t pid, ProcessInfo& info) override;

    // Find processes by name (partial match)
    std::vector<ProcessInfo> FindProcessesByName(const std::string& name) override;

    // Get OS name
    const char* GetOSName() const override { return "Windows"; }

    // Get kernel version if available
    std::string GetKernelVersion() const override;

    // Windows-specific: Set custom kernel offsets
    void SetKernelOffsets(const WindowsKernelOffsets& offsets);

    // Windows-specific: Try to auto-detect kernel offsets
    bool AutoDetectOffsets();

    // Windows-specific: Get System process (PID 4) EPROCESS address
    uint64_t GetSystemProcessAddress() const { return systemProcessAddr; }

    // Windows-specific: Get Idle process (PID 0) EPROCESS address
    uint64_t GetIdleProcessAddress() const { return idleProcessAddr; }

private:
    uint64_t systemProcessAddr;  // PID 4 - System process
    uint64_t idleProcessAddr;    // PID 0 - Idle process
    uint64_t psActiveHead;        // PsActiveProcessHead
    WindowsKernelOffsets offsets;
    std::string windowsVersion;
    bool offsetsDetected;

    // Common offset configurations for different Windows versions
    static const std::vector<WindowsKernelOffsets> KNOWN_OFFSET_CONFIGS;

    // Helper to read EPROCESS fields
    bool ReadEProcess(uint64_t eprocAddr, ProcessInfo& info);

    // Helper to validate a potential EPROCESS
    bool ValidateEProcess(uint64_t addr);

    // Helper to find System process through various methods
    bool FindSystemProcess();

    // Helper to walk the process list
    std::vector<ProcessInfo> WalkProcessList(uint64_t startEproc);

    // Helper to read process name from EPROCESS
    std::string ReadProcessName(uint64_t eprocAddr);

    // Windows version detection
    void DetectWindowsVersion();
};

}