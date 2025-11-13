#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace Haywire {

/**
 * Abstract interface for OS-specific kernel discovery
 *
 * This interface provides a common API for discovering processes and kernel
 * structures across different guest operating systems (Linux, Windows, etc.)
 */
class IKernelDiscovery {
public:
    // Common structures shared across all OS types

    struct MemorySection {
        enum Type {
            UNKNOWN = 0,
            ANONYMOUS,      // Heap, anonymous mmap
            FILE_BACKED,    // Mapped file (not code/lib)
            SHARED_LIB,     // Shared library (.so/.dll)
            EXECUTABLE,     // Executable code
            STACK,          // Thread stack
            HEAP,           // Heap pages
            VDSO,           // Virtual DSO (Linux) / VDLL (Windows)
            VVAR,           // Virtual var (Linux)
        };

        uint64_t start;             // Start virtual address
        uint64_t end;               // End virtual address
        uint64_t flags;             // Memory protection flags (OS-specific)
        std::string name;           // Mapped file name (if any)
        Type type;                  // Classification of this section

        MemorySection() : start(0), end(0), flags(0), type(UNKNOWN) {}
    };

    struct PTE {
        uint64_t va;                // Virtual address
        uint64_t pa;                // Physical address
        uint64_t size;              // Page size (4KB, 2MB, 1GB)
        bool present;
        bool writable;
        bool executable;
    };

    struct ProcessInfo {
        uint64_t task_addr;         // Physical address of process structure
        uint32_t pid;               // Process ID
        uint32_t tgid;              // Thread group ID (Linux) / Process ID (Windows)
        std::string comm;           // Process name
        uint64_t mm_addr;           // Kernel VA of memory descriptor (or 0 for kernel threads)
        uint64_t pgd;               // Page Global Directory physical address
        bool has_mm;                // Has memory descriptor
        bool is_kernel_thread;      // Kernel thread (no user address space)
        std::vector<MemorySection> sections;  // Memory regions
        std::vector<PTE> ptes;      // Page table entries (if walked)
    };

    struct KernelInfo {
        uint64_t swapper_pgd;       // Kernel PGD (swapper_pg_dir on Linux, DirectoryTableBase for System on Windows)
        uint64_t init_task;         // Init process address (task_struct for PID 1 / System EPROCESS)
        uint64_t current_task;      // Current task on CPU
    };

    virtual ~IKernelDiscovery() = default;

    // Lifecycle methods
    virtual bool Initialize() = 0;
    virtual void Cleanup() = 0;

    // Discovery methods
    virtual bool DiscoverKernel() = 0;
    virtual bool DiscoverProcesses() = 0;

    // Access methods
    virtual const std::vector<ProcessInfo>& GetProcesses() const = 0;
    virtual const KernelInfo& GetKernelInfo() const = 0;

    // Per-process operations
    virtual void WalkProcessPageTables(ProcessInfo& proc) = 0;
    virtual bool ExtractProcessMemoryMap(uint32_t pid) = 0;
    virtual void ExtractProcessPGDs() = 0;

    // Virtual address translation
    virtual uint64_t TranslateVA(uint64_t va, uint64_t pgdBase) = 0;

    // OS type identification
    virtual const char* GetOSType() const = 0;  // "Linux", "Windows", etc.
    virtual const char* GetArchitecture() const = 0;  // "x86_64", "aarch64"
};

/**
 * Guest OS detection
 */
enum class GuestOS {
    Unknown = 0,
    Linux,
    Windows,
    FreeBSD,
    MacOS
};

/**
 * Factory function to create appropriate kernel discovery implementation
 *
 * @param memoryFile Path to memory file (QEMU memory-backend-file or VMware .vmem)
 * @param profilePath Path to kernel profile JSON (optional)
 * @param osHint OS hint for detection (optional, auto-detected if Unknown)
 * @return Unique pointer to IKernelDiscovery implementation
 */
std::unique_ptr<IKernelDiscovery> CreateKernelDiscovery(
    const std::string& memoryFile,
    const std::string& profilePath = "",
    GuestOS osHint = GuestOS::Unknown
);

/**
 * Detect guest OS from memory signatures
 *
 * @param memoryFile Path to memory file
 * @return Detected GuestOS enum
 */
GuestOS DetectGuestOS(const std::string& memoryFile);

} // namespace Haywire
