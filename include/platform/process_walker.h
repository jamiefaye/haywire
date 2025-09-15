#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace Haywire {

class MemoryBackend;
class PageWalker;

// Structure representing a process
struct ProcessInfo {
    uint64_t pid;
    std::string name;
    uint64_t task_struct_addr;  // Linux: task_struct address
    uint64_t eprocess_addr;      // Windows: EPROCESS address
    uint64_t page_table_base;    // CR3/TTBR0
    uint64_t mm_struct_addr;     // Linux: mm_struct address
    uint64_t peb_addr;           // Windows: PEB address

    // Additional fields can be added as needed
    uint64_t parent_pid;
    uint64_t thread_count;
    uint64_t virtual_size;
};

// Abstract base class for process enumeration
// Platform-specific implementations for Linux/Windows
class ProcessWalker {
public:
    ProcessWalker(MemoryBackend* backend) : memory(backend) {}
    virtual ~ProcessWalker() = default;

    // Initialize the walker (find init_task/PsInitialSystemProcess)
    virtual bool Initialize() = 0;

    // Walk the process list and return all processes
    virtual std::vector<ProcessInfo> EnumerateProcesses() = 0;

    // Find a specific process by PID
    virtual bool FindProcess(uint64_t pid, ProcessInfo& info) = 0;

    // Find processes by name (partial match)
    virtual std::vector<ProcessInfo> FindProcessesByName(const std::string& name) = 0;

    // Get OS name for debugging
    virtual const char* GetOSName() const = 0;

    // Get kernel version if available
    virtual std::string GetKernelVersion() const { return "Unknown"; }

protected:
    MemoryBackend* memory;

    // Helper to read a string from memory
    std::string ReadString(uint64_t addr, size_t maxLen = 256);

    // Helper to read a null-terminated string
    std::string ReadCString(uint64_t addr, size_t maxLen = 256);
};

// Factory function to create appropriate process walker
std::unique_ptr<ProcessWalker> CreateProcessWalker(MemoryBackend* backend,
                                                   const std::string& os);

}