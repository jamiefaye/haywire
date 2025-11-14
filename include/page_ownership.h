#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace Haywire {

/**
 * PageOwnership - Classification of who "owns" a VM page
 *
 * This system allows us to identify what each page in a process's
 * virtual address space is used for, enabling filtering, sorting,
 * and visualization by page type.
 */
struct PageOwnership {
    enum Type {
        UNKNOWN = 0,
        ANONYMOUS,           // Heap, stack, anonymous mmap
        FILE_BACKED,         // Mapped file (not code/lib)
        SHARED_LIB,          // Shared library (.so)
        EXECUTABLE,          // Executable code
        STACK,               // Thread stack
        HEAP,                // Heap pages
        VDSO,                // Virtual DSO
        VVAR,                // Virtual var
        VSYSCALL,            // Virtual syscall page
        UNMAPPED             // Not present/accessible
    };

    Type type = UNKNOWN;
    std::string filename;    // File path if file-backed
    uint64_t fileOffset = 0; // Offset within file
    uint64_t vmaStart = 0;   // VMA this page belongs to
    uint64_t vmaEnd = 0;
    uint64_t flags = 0;      // VM_READ, VM_WRITE, VM_EXEC, VM_SHARED
    int pid = 0;             // Owner PID
    bool present = false;    // Page currently mapped (from PTE)
    bool dirty = false;      // Modified since load (from PTE)
    bool accessed = false;   // Recently accessed (from PTE)

    // VM flag bits (from Linux kernel)
    static constexpr uint64_t VM_READ    = 0x00000001;
    static constexpr uint64_t VM_WRITE   = 0x00000002;
    static constexpr uint64_t VM_EXEC    = 0x00000004;
    static constexpr uint64_t VM_SHARED  = 0x00000008;
    static constexpr uint64_t VM_MAYREAD = 0x00000010;
    static constexpr uint64_t VM_MAYWRITE= 0x00000020;
    static constexpr uint64_t VM_MAYEXEC = 0x00000040;
    static constexpr uint64_t VM_MAYSHARE= 0x00000080;
    static constexpr uint64_t VM_GROWSDOWN=0x00000100;
    static constexpr uint64_t VM_PFNMAP  = 0x00000400;
    static constexpr uint64_t VM_DENYWRITE=0x00000800;
    static constexpr uint64_t VM_EXECUTABLE=0x00001000;

    // Helper methods
    bool isReadable() const { return flags & VM_READ; }
    bool isWritable() const { return flags & VM_WRITE; }
    bool isExecutable() const { return flags & VM_EXEC; }
    bool isShared() const { return flags & VM_SHARED; }

    // Get a human-readable type name
    const char* getTypeName() const {
        switch (type) {
            case UNKNOWN: return "Unknown";
            case ANONYMOUS: return "Anonymous";
            case FILE_BACKED: return "File-backed";
            case SHARED_LIB: return "Shared Library";
            case EXECUTABLE: return "Executable";
            case STACK: return "Stack";
            case HEAP: return "Heap";
            case VDSO: return "vDSO";
            case VVAR: return "vvar";
            case VSYSCALL: return "vsyscall";
            case UNMAPPED: return "Unmapped";
            default: return "???";
        }
    }

    // Get a color for visualization (RGBA packed)
    uint32_t getColor() const {
        switch (type) {
            case EXECUTABLE:  return 0xFF0000FF;  // Red
            case SHARED_LIB:  return 0xFF00FFFF;  // Cyan
            case FILE_BACKED: return 0xFF00FF00;  // Green
            case ANONYMOUS:   return 0xFF00AAFF;  // Yellow
            case STACK:       return 0xFF0088FF;  // Orange
            case HEAP:        return 0xFF00DDFF;  // Light yellow
            case VDSO:        return 0xFFFF00FF;  // Magenta
            case VVAR:        return 0xFFFF0080;  // Purple
            case UNMAPPED:    return 0xFF000000;  // Black
            default:          return 0xFF808080;  // Gray
        }
    }
};

/**
 * PageOwnershipClassifier - Determines ownership from VMA information
 */
class PageOwnershipClassifier {
public:
    /**
     * Classify a VMA into page ownership type
     *
     * @param vmaStart Virtual address range start
     * @param vmaEnd Virtual address range end
     * @param vmFlags VM flags from vm_area_struct
     * @param filename Filename if file-backed (empty if anonymous)
     * @param vmaName Region name from /proc/pid/maps notation (optional)
     * @return Classified page ownership type
     */
    static PageOwnership::Type Classify(
        uint64_t vmaStart,
        uint64_t vmaEnd,
        uint64_t vmFlags,
        const std::string& filename,
        const std::string& vmaName = ""
    );

    /**
     * Check if filename indicates shared library
     */
    static bool IsSharedLibrary(const std::string& filename);

    /**
     * Check if filename indicates executable
     */
    static bool IsExecutable(const std::string& filename, uint64_t vmFlags);

    /**
     * Extract base name from full path
     */
    static std::string BaseName(const std::string& path);
};

/**
 * PageOwnershipMap - Maps virtual addresses to ownership info
 */
class PageOwnershipMap {
public:
    PageOwnershipMap(int pid) : pid_(pid) {}

    /**
     * Add a VMA with its ownership classification
     */
    void AddVMA(uint64_t start, uint64_t end, const PageOwnership& ownership);

    /**
     * Lookup ownership for a virtual address
     */
    const PageOwnership* Lookup(uint64_t virtualAddr) const;

    /**
     * Get all VMAs for this process
     */
    const std::vector<PageOwnership>& GetVMAs() const { return vmas_; }

    /**
     * Update PTE information (present, dirty, accessed bits)
     */
    void UpdatePTEInfo(uint64_t virtualAddr, bool present, bool dirty, bool accessed);

    /**
     * Get statistics about ownership distribution
     */
    struct Stats {
        size_t totalPages = 0;
        size_t presentPages = 0;
        std::map<PageOwnership::Type, size_t> pagesByType;
        std::map<std::string, size_t> pagesByFile;
    };
    Stats GetStats() const;

    int GetPID() const { return pid_; }

private:
    int pid_;
    std::vector<PageOwnership> vmas_;  // Sorted by start address

    // Binary search helper
    const PageOwnership* BinarySearch(uint64_t addr) const;
};

} // namespace Haywire
