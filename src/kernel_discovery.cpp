/**
 * Kernel Discovery for Haywire
 *
 * Discovers kernel structures and process information using QMP and memory scanning
 * Replaces the need for companion processes or guest agents
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <errno.h>
#include "platform_compat.h"
#include "../include/qemu_connection.h"
#include "kernel_profile_loader.h"

namespace Haywire {

// Kernel structure offsets - can be loaded from profile JSON or use defaults
struct KernelOffsets {
    // task_struct offsets (verified with pahole)
    size_t tasks_list = 0x680;     // struct list_head tasks at 1664
    size_t tasks_next = 0x680;     // tasks.next
    size_t tasks_prev = 0x688;     // tasks.prev (tasks_list + 8)
    size_t pid = 0x750;            // PID_OFFSET
    size_t comm = 0x970;           // COMM_OFFSET
    size_t mm = 0x6d0;             // MM_OFFSET

    // mm_struct offsets
    size_t mm_pgd = 0x68;          // pgd (page global directory)
    size_t mm_mt = 0x40;           // maple tree root
    size_t mm_users = 0x74;        // mm_users refcount

    // maple tree node offsets
    size_t mt_root = 0x48;         // Actual root node pointer at offset 8 in maple_tree

    // vm_area_struct offsets (in maple tree leaves)
    // Updated for kernel 6.14.0-34 (offsets changed from -32)
    size_t vma_start = 0x00;       // vm_start
    size_t vma_end = 0x08;         // vm_end
    size_t vma_next = 0x10;        // vm_next (linked list)
    size_t vma_flags = 0x20;       // vm_flags (was 0x50, changed in 6.14.0-34)
    size_t vma_file = 0x80;        // vm_file pointer (was 0x90, changed in 6.14.0-34)

    // Load from profile JSON
    void LoadFromProfile(const KernelProfile& profile) {
        tasks_list = profile.task_tasks;
        tasks_next = profile.task_tasks;
        tasks_prev = profile.task_tasks + 8;
        pid = profile.task_pid;
        comm = profile.task_comm;
        mm = profile.task_mm;

        mm_pgd = profile.mm_pgd;
        mm_mt = profile.mm_mt;
        mm_users = profile.mm_users;

        vma_start = profile.vma_start;
        vma_end = profile.vma_end;
        vma_next = profile.vma_next;
        vma_flags = profile.vma_flags;
        vma_file = profile.vma_file;

        std::cout << "Loaded offsets from kernel profile" << std::endl;
    }
};

// Known process names from web version
const std::vector<std::string> KNOWN_PROCESSES = {
    "systemd", "init", "kthreadd", "kworker", "ksoftirqd", "migration",
    "rcu_", "sshd", "bash", "NetworkManager", "dbus", "cron", "systemd-",
    "kswapd", "kauditd", "kcompactd", "khugepaged", "systemd-journal",
    "systemd-resolved", "systemd-networkd", "vlc", "firefox", "chrome"
};

class KernelDiscovery {
public:
    struct MemorySection {
        enum Type {
            UNKNOWN = 0,
            ANONYMOUS,      // Heap, anonymous mmap
            FILE_BACKED,    // Mapped file (not code/lib)
            SHARED_LIB,     // Shared library (.so)
            EXECUTABLE,     // Executable code
            STACK,          // Thread stack
            HEAP,           // Heap pages
            VDSO,           // Virtual DSO
            VVAR,           // Virtual var
        };

        uint64_t start;             // Start virtual address
        uint64_t end;               // End virtual address
        uint64_t flags;             // VM flags
        std::string name;           // Mapped file name (if any)
        Type type;                  // Classification of this section

        MemorySection() : start(0), end(0), flags(0), type(UNKNOWN) {}
    };

    struct PTE {
        uint64_t va;                // Virtual address
        uint64_t pa;                // Physical address
        uint64_t size;              // Page size
        bool present;
        bool writable;
        bool executable;
    };

    struct ProcessInfo {
        uint64_t task_addr;        // Physical address of task_struct
        uint32_t pid;
        uint32_t tgid;
        std::string comm;
        uint64_t mm_addr;           // Kernel VA of mm_struct (or 0 for kernel threads)
        uint64_t pgd;               // Page Global Directory physical address
        bool has_mm;
        bool is_kernel_thread;      // mm == 0 indicates kernel thread
        std::vector<MemorySection> sections;  // Memory sections from maple tree
        std::vector<PTE> ptes;      // Page table entries
    };

    struct KernelInfo {
        uint64_t swapper_pgd;       // Kernel PGD from TTBR1
        uint64_t init_task;         // init_task address
        uint64_t current_task;      // Current task on CPU
        KernelOffsets offsets;      // Detected/configured offsets
    };

    KernelDiscovery(const std::string& memFile = "/tmp/haywire-vm-mem",
                    const std::string& profilePath = "")
        : memoryFile(memFile),
          kernelProfilePath(profilePath),
          memFd(-1), memBase(nullptr) {}

    ~KernelDiscovery() {
        Cleanup();
    }

    bool Initialize() {
        // Load kernel profile if specified
        if (!kernelProfilePath.empty()) {
            KernelProfile profile;
            if (KernelProfileLoader::LoadProfile(kernelProfilePath, profile)) {
                kernelInfo.offsets.LoadFromProfile(profile);
                std::cout << "Using kernel profile: " << kernelProfilePath << std::endl;
            } else {
                std::cerr << "Warning: Failed to load profile, using default offsets" << std::endl;
            }
        } else {
            // Try to auto-detect profile
            std::string autoProfile = KernelProfileLoader::DetectProfile("profiles");
            if (!autoProfile.empty()) {
                KernelProfile profile;
                if (KernelProfileLoader::LoadProfile(autoProfile, profile)) {
                    kernelInfo.offsets.LoadFromProfile(profile);
                    std::cout << "Auto-detected kernel profile: " << autoProfile << std::endl;
                }
            } else {
                std::cout << "No kernel profile specified, using built-in defaults" << std::endl;
            }
        }

        // Open memory file
        memFd = open(memoryFile.c_str(), O_RDONLY);
        if (memFd < 0) {
            std::cerr << "Failed to open memory file: " << memoryFile << std::endl;
            return false;
        }

        // Get file size
        off_t fileSize = lseek(memFd, 0, SEEK_END);
        if (fileSize < 0) {
            std::cerr << "Failed to get file size" << std::endl;
            close(memFd);
            return false;
        }
        memorySize = fileSize;

        // Memory map the file for fast access
        memBase = mmap(nullptr, memorySize, PROT_READ, MAP_PRIVATE, memFd, 0);
        if (memBase == MAP_FAILED) {
            std::cerr << "Failed to mmap memory file" << std::endl;
            close(memFd);
            return false;
        }

        std::cout << "Memory mapped: " << (memorySize / (1024*1024)) << " MB" << std::endl;

        // QMP connection will be handled via singleton from main program
        return true;
    }

    bool LoadCachedSwapperPGD() {
        std::ifstream cache("/tmp/haywire-swapper-pgd.txt");
        if (!cache.is_open()) {
            return false;
        }

        cache >> std::hex >> kernelInfo.swapper_pgd >> kernelInfo.current_task;
        if (!cache.good()) {
            return false;
        }

        std::cout << "Loaded cached swapper PGD: 0x" << std::hex
                  << kernelInfo.swapper_pgd << std::dec << std::endl;
        std::cout << "Loaded cached current_task: 0x" << std::hex
                  << kernelInfo.current_task << std::dec << std::endl;
        return true;
    }

    void SaveSwapperPGDCache() {
        std::ofstream cache("/tmp/haywire-swapper-pgd.txt");
        if (cache.is_open()) {
            cache << std::hex << kernelInfo.swapper_pgd << " "
                  << kernelInfo.current_task << std::endl;
            std::cout << "Cached swapper PGD to /tmp/haywire-swapper-pgd.txt" << std::endl;
        }
    }

    bool DiscoverKernel() {
        std::cout << "\n=== Kernel Discovery ===" << std::endl;

        // Try loading from cache first (allows multiple concurrent instances)
        if (LoadCachedSwapperPGD()) {
            std::cout << "Using cached kernel info (QMP not needed)" << std::endl;
            return true;
        }

        std::cout << "No cache found, querying via QMP..." << std::endl;

        // Cache miss - need to query via QMP
        QemuConnection& qemu = QemuConnection::getInstance();
        if (qemu.IsQMPConnected()) {
            std::cout << "Using shared QMP connection for kernel info..." << std::endl;
            if (qemu.QueryKernelInfo(0, kernelInfo.swapper_pgd, kernelInfo.current_task)) {
                // Successfully got kernel info from QMP
                std::cout << "QMP query successful" << std::endl;
                std::cout << "Got swapper PGD from QMP: 0x" << std::hex << kernelInfo.swapper_pgd << std::dec << std::endl;
                std::cout << "Got current_task from QMP: 0x" << std::hex << kernelInfo.current_task << std::dec << std::endl;

                // Cache for next time (allows subsequent instances to skip QMP)
                SaveSwapperPGDCache();

                return true;
            } else {
                std::cerr << "ERROR: QMP query failed - cannot get swapper PGD" << std::endl;
                std::cerr << "Cannot proceed without kernel information from QMP" << std::endl;
                return false;
            }
        } else {
            std::cerr << "QMP not connected - will try heuristic discovery after process scan" << std::endl;
            std::cerr << "Note: Heuristic discovery requires processes to be discovered first" << std::endl;

            // Don't fail here - we'll discover swapper_pgd after process scan
            // Set a flag to indicate we need heuristic discovery
            kernelInfo.swapper_pgd = 0;  // Mark as not yet discovered
            return true;  // Continue anyway
        }
    }

    bool DiscoverProcesses() {
        std::cout << "\n=== Process Discovery ===" << std::endl;

        // Always do full scan to catch newly started processes (like Firefox)
        // The "fast refresh" approach only checks previous locations and misses new PIDs
        std::cout << "Full memory scan for task_structs..." << std::endl;
        return DiscoverProcessesFullScan();
    }

    bool DiscoverProcessesFullScan() {
        // Clear any previous data
        suspectLocations.clear();

        processes.clear();
        std::set<uint32_t> seenPids;

        // SLAB offsets where task_structs are commonly found (from web version)
        const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};  // 32KB SLAB positions
        const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};  // Page-straddle offsets
        const uint64_t PAGE_SIZE = 4096;
        const uint64_t TASK_STRUCT_SIZE = 9088;  // Exact size from web version

        // Scan entire memory
        uint64_t scannedMB = 0;
        uint32_t lastPid = 0;
        for (uint64_t pageStart = 0; pageStart < memorySize; pageStart += PAGE_SIZE) {
            // Progress report every 1GB
            if (pageStart % (1024 * 1024 * 1024) == 0) {
                scannedMB = pageStart / (1024 * 1024);
                std::cout << "  Scanned " << scannedMB << "MB... ("
                          << processes.size() << " processes found)\r" << std::flush;
            }

            // Try both SLAB offsets and PAGE_STRADDLE offsets
            std::vector<uint64_t> offsetsToCheck;
            for (auto off : SLAB_OFFSETS) offsetsToCheck.push_back(off);
            for (auto off : PAGE_STRADDLE_OFFSETS) offsetsToCheck.push_back(off);

            for (const auto slabOffset : offsetsToCheck) {
                uint64_t offset = pageStart + slabOffset;
                if (offset + TASK_STRUCT_SIZE > memorySize) {
                    continue;
                }

                ProcessInfo proc;
                if (CheckTaskStruct(offset, proc)) {
                    // Remember this location as a suspect for fast refresh
                    uint64_t pa = offset + 0x40000000;
                    suspectLocations.insert(pa);

                    // Avoid duplicates and consecutive identical PIDs (likely same task)
                    if (seenPids.find(proc.pid) == seenPids.end()) {
                        seenPids.insert(proc.pid);
                        processes.push_back(proc);

                        // Debug first few valid processes
                        if (processes.size() <= 5) {
                            std::cout << "\n  Found PID " << proc.pid << " (" << proc.comm << ")"
                                      << " at file offset 0x" << std::hex << offset
                                      << " (PA 0x" << pa << ")" << std::dec;
                        }
                    }
                }
            }
        }

        // Count kernel vs user processes
        int kernelThreads = 0;
        int userProcesses = 0;
        for (const auto& proc : processes) {
            if (proc.is_kernel_thread) kernelThreads++;
            else userProcesses++;
        }

        std::cout << "\nFound " << processes.size() << " unique processes:" << std::endl;
        std::cout << "  Kernel threads (mm==0): " << kernelThreads << std::endl;
        std::cout << "  User processes (mm!=0): " << userProcesses << std::endl;

        // Display first 20 processes
        std::cout << "\nFirst 20 processes:" << std::endl;
        std::cout << "PID     | Name             | Type   | PGD" << std::endl;
        std::cout << "--------|------------------|--------|----------------" << std::endl;

        int displayed = 0;
        for (const auto& proc : processes) {
            if (displayed >= 20) break;

            std::cout << std::setw(7) << proc.pid
                      << " | " << std::setw(16) << proc.comm
                      << " | " << (proc.is_kernel_thread ? "Kernel" : "User  ")
                      << " | ";

            if (proc.pgd) {
                std::cout << "0x" << std::hex << proc.pgd << std::dec;
            } else {
                std::cout << "NULL";
            }
            std::cout << std::endl;
            displayed++;
        }

        // Mark initial scan as complete
        hasInitialScan = true;
        std::cout << "\nInitial scan complete. Found " << suspectLocations.size()
                  << " suspect locations for fast refresh." << std::endl;

        return !processes.empty();
    }

    bool RefreshFromSuspects() {
        // Fast refresh: only check previously found locations
        auto startTime = std::chrono::steady_clock::now();

        processes.clear();
        std::set<uint32_t> seenPids;
        int alive = 0, changed = 0, dead = 0;

        for (uint64_t pa : suspectLocations) {
            uint64_t offset = pa - 0x40000000;
            if (offset >= memorySize) continue;

            ProcessInfo proc;
            if (CheckTaskStruct(offset, proc)) {
                alive++;
                // Avoid duplicates
                if (seenPids.find(proc.pid) == seenPids.end()) {
                    seenPids.insert(proc.pid);
                    processes.push_back(proc);
                }
            } else {
                dead++;
            }
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        std::cout << "Fast refresh complete in " << duration.count() << "ms:" << std::endl;
        std::cout << "  Checked " << suspectLocations.size() << " locations" << std::endl;
        std::cout << "  Found " << processes.size() << " unique processes" << std::endl;
        std::cout << "  Alive: " << alive << ", Dead: " << dead << std::endl;

        return !processes.empty();
    }

    const std::vector<ProcessInfo>& GetProcesses() const { return processes; }
    const KernelInfo& GetKernelInfo() const { return kernelInfo; }

    // Walk page tables for a process to extract PTEs
    void WalkProcessPageTables(ProcessInfo& proc) {
        if (!proc.pgd || proc.is_kernel_thread) return;

        // Walk all PGD entries - with ASLR, user space can use any index
        for (int pgdIdx = 0; pgdIdx < 512; pgdIdx++) {
            uint64_t pgdOffset = (proc.pgd - 0x40000000) + (pgdIdx * 8);
            if (pgdOffset + 8 > memorySize) continue;

            uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);
            if (!pgdEntry || (pgdEntry & 3) == 0) continue;

            // Walk through PUD, PMD, PTE levels
            WalkPudLevel(pgdEntry, pgdIdx, proc.ptes);
        }
    }

    void WalkPudLevel(uint64_t pudTableAddr, int pgdIdx, std::vector<PTE>& ptes) {
        // Extract physical address from page table entry (bits [47:12])
        uint64_t pudBase = pudTableAddr & 0x0000FFFFFFFFF000ULL;

        for (int pudIdx = 0; pudIdx < 512; pudIdx++) {
            uint64_t pudPhysAddr = pudBase + (pudIdx * 8);
            uint64_t pudOffset = pudPhysAddr >= 0x40000000
                ? pudPhysAddr - 0x40000000
                : pudPhysAddr;

            if (pudOffset + 8 > memorySize) continue;
            uint64_t pudEntry = *(uint64_t*)((uint8_t*)memBase + pudOffset);

            if (!pudEntry || (pudEntry & 3) == 0) continue;

            uint32_t entryType = pudEntry & 3;
            if (entryType == 1) {
                // 1GB huge page
                uint64_t va = ((uint64_t)pgdIdx << 39) | ((uint64_t)pudIdx << 30);

                // Skip kernel VAs (those with bits 63-48 all set)
                if ((va >> 48) == 0xFFFF) continue;

                uint32_t pudFlags = pudEntry & 0xFFF;
                uint64_t pa = pudEntry & 0x0000FFFFC0000000ULL;

                PTE pte;
                pte.va = va;
                pte.pa = pa;
                pte.size = 0x40000000;  // 1GB
                pte.present = (pudFlags & 1) != 0;
                pte.writable = (pudFlags & 0x80) == 0;
                pte.executable = (pudFlags & 0x10) == 0;
                ptes.push_back(pte);
            } else if (entryType == 3) {
                // Table descriptor, continue to PMD
                WalkPmdLevel(pudEntry, pgdIdx, pudIdx, ptes);
            }
        }
    }

    void WalkPmdLevel(uint64_t pmdTableAddr, int pgdIdx, int pudIdx, std::vector<PTE>& ptes) {
        // Extract physical address from page table entry (bits [47:12])
        uint64_t pmdBase = pmdTableAddr & 0x0000FFFFFFFFF000ULL;

        for (int pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            uint64_t pmdPhysAddr = pmdBase + (pmdIdx * 8);
            uint64_t pmdOffset = pmdPhysAddr >= 0x40000000
                ? pmdPhysAddr - 0x40000000
                : pmdPhysAddr;

            if (pmdOffset + 8 > memorySize) continue;
            uint64_t pmdEntry = *(uint64_t*)((uint8_t*)memBase + pmdOffset);

            if (!pmdEntry || (pmdEntry & 3) == 0) continue;

            uint32_t entryType = pmdEntry & 3;
            if (entryType == 1) {
                // 2MB huge page
                uint64_t va = ((uint64_t)pgdIdx << 39) | ((uint64_t)pudIdx << 30) | ((uint64_t)pmdIdx << 21);

                // Skip kernel VAs
                if ((va >> 48) == 0xFFFF) continue;

                uint32_t pmdFlags = pmdEntry & 0xFFF;
                uint64_t pa = pmdEntry & 0x0000FFFFFFE00000ULL;

                PTE pte;
                pte.va = va;
                pte.pa = pa;
                pte.size = 0x200000;  // 2MB
                pte.present = (pmdFlags & 1) != 0;
                pte.writable = (pmdFlags & 0x80) == 0;
                pte.executable = (pmdFlags & 0x10) == 0;
                ptes.push_back(pte);
            } else if (entryType == 3) {
                // Table descriptor, continue to PTE
                WalkPteLevel(pmdEntry, pgdIdx, pudIdx, pmdIdx, ptes);
            }
        }
    }

    void WalkPteLevel(uint64_t pteTableAddr, int pgdIdx, int pudIdx, int pmdIdx, std::vector<PTE>& ptes) {
        // Extract physical address from page table entry (bits [47:12])
        uint64_t pteBase = pteTableAddr & 0x0000FFFFFFFFF000ULL;

        for (int pteIdx = 0; pteIdx < 512; pteIdx++) {
            uint64_t ptePhysAddr = pteBase + (pteIdx * 8);
            uint64_t pteOffset = ptePhysAddr >= 0x40000000
                ? ptePhysAddr - 0x40000000
                : ptePhysAddr;

            if (pteOffset + 8 > memorySize) continue;
            uint64_t pteEntry = *(uint64_t*)((uint8_t*)memBase + pteOffset);

            if (!pteEntry || (pteEntry & 3) == 0) continue;

            // Regular 4KB page
            uint64_t va = ((uint64_t)pgdIdx << 39) | ((uint64_t)pudIdx << 30) |
                          ((uint64_t)pmdIdx << 21) | ((uint64_t)pteIdx << 12);

            // With modern ARM64 + ASLR, user processes can use high addresses
            // Only skip actual kernel space addresses (0xFFFF...)
            if ((va >> 48) == 0xFFFF) continue;

            uint32_t pteFlags = pteEntry & 0xFFF;
            uint64_t pa = pteEntry & 0x0000FFFFFFFFF000ULL;

            PTE pte;
            pte.va = va;
            pte.pa = pa;
            pte.size = 0x1000;  // 4KB
            pte.present = (pteFlags & 1) != 0;
            pte.writable = (pteFlags & 0x80) == 0;
            pte.executable = (pteFlags & 0x10) == 0;
            ptes.push_back(pte);
        }
    }

    // Extract PGD for each process
    void ExtractProcessPGDs() {
        std::cout << "\n=== Extracting Process PGDs ===" << std::endl;

        int successCount = 0;
        int failCount = 0;

        // Check if we have swapper PGD from QMP/cache
        uint64_t qmpSwapperPGD = kernelInfo.swapper_pgd;

        if (qmpSwapperPGD == 0) {
            std::cout << "No swapper PGD from QMP - attempting heuristic discovery..." << std::endl;
            kernelInfo.swapper_pgd = FindSwapperPGD();

            if (kernelInfo.swapper_pgd == 0) {
                std::cerr << "ERROR: Heuristic discovery failed - cannot translate kernel VAs" << std::endl;
                return;
            }

            // Cache the discovered value for future use
            SaveSwapperPGDCache();
        } else {
            // We have QMP value - run heuristic for comparison/validation
            std::cout << "Validating QMP swapper PGD with heuristic discovery..." << std::endl;
            uint64_t heuristicPGD = FindSwapperPGD();

            if (heuristicPGD != 0 && heuristicPGD != qmpSwapperPGD) {
                std::cerr << "\n=== WARNING: Swapper PGD mismatch! ===" << std::endl;
                std::cerr << "QMP says:       0x" << std::hex << qmpSwapperPGD << std::dec << std::endl;
                std::cerr << "Heuristic says: 0x" << std::hex << heuristicPGD << std::dec << std::endl;
                std::cerr << "Using QMP value (more authoritative)" << std::endl;
                std::cerr << "====================================\n" << std::endl;
            } else if (heuristicPGD == qmpSwapperPGD) {
                std::cout << "✓ Heuristic validation confirms QMP swapper PGD" << std::endl;
            }
        }

        std::cout << "Using swapper PGD: 0x" << std::hex << kernelInfo.swapper_pgd << std::dec << std::endl;

        for (auto& proc : processes) {
            if (proc.is_kernel_thread) continue;  // Skip kernel threads

            // Debug first few
            if (failCount < 3) {
                std::cout << "\nTranslating mm_struct for PID " << proc.pid << " (" << proc.comm << ")" << std::endl;
                std::cout << "  mm_struct VA: 0x" << std::hex << proc.mm_addr << std::dec << std::endl;
            }

            // Translate mm_struct VA to PA using swapper PGD
            uint64_t mmPA = TranslateVA(proc.mm_addr, kernelInfo.swapper_pgd);
            if (!mmPA) {
                if (failCount < 3) {
                    std::cout << "  Failed to translate mm_struct VA to PA" << std::endl;
                }
                failCount++;
                continue;
            }

            if (successCount < 3) {
                std::cout << "  mm_struct PA: 0x" << std::hex << mmPA << std::dec << std::endl;
            }

            // Read PGD from mm_struct at offset 0x68
            uint64_t mmOffset = mmPA - 0x40000000;
            if (mmOffset + kernelInfo.offsets.mm_pgd + 8 > memorySize) {
                failCount++;
                continue;
            }

            uint64_t pgdVA = *(uint64_t*)((uint8_t*)memBase + mmOffset + kernelInfo.offsets.mm_pgd);

            // The PGD is stored as a kernel VA, translate it to PA
            uint64_t pgdPA = TranslateVA(pgdVA, kernelInfo.swapper_pgd);
            if (!pgdPA) {
                if (failCount < 3) {
                    std::cout << "  Failed to translate PGD VA 0x" << std::hex << pgdVA
                              << " to PA" << std::dec << std::endl;
                }
                failCount++;
                continue;
            }

            proc.pgd = pgdPA;  // Store the physical address

            // Try to walk maple tree for memory sections
            std::cout << "[Discovery] PID " << proc.pid << " calling WalkMapleTree with mmPA=0x"
                      << std::hex << mmPA << std::dec << "\n" << std::flush;
            WalkMapleTree(mmPA, proc.sections);
            std::cout << "[Discovery] PID " << proc.pid << " got " << proc.sections.size() << " sections\n" << std::flush;

            // Walk page tables to extract PTEs
            WalkProcessPageTables(proc);

            successCount++;

            // Show first few
            if (successCount <= 5) {
                std::cout << "PID " << proc.pid << " (" << proc.comm << "): PGD = 0x"
                          << std::hex << proc.pgd << std::dec << " (PA)";
                if (!proc.sections.empty()) {
                    std::cout << ", " << proc.sections.size() << " memory sections";
                }
                std::cout << std::endl;
            }
        }

        std::cout << "\nExtracted PGDs: " << successCount << " success, "
                  << failCount << " failed" << std::endl;

        // Auto-recovery: If success rate is very low, swapper PGD might be stale
        int totalAttempted = successCount + failCount;
        if (totalAttempted > 10 && successCount < totalAttempted / 10) {
            // Less than 10% success rate - swapper PGD is likely wrong
            std::cerr << "\n=== WARNING: Very low success rate (" << successCount << "/" << totalAttempted
                      << ") - swapper PGD may be stale ===" << std::endl;
            std::cerr << "Attempting to refresh swapper PGD from QMP..." << std::endl;

            // Delete cache and retry discovery
            std::remove("/tmp/haywire-swapper-pgd.txt");

            QemuConnection& qemu = QemuConnection::getInstance();
            if (qemu.IsQMPConnected()) {
                uint64_t newSwapperPGD = 0;
                uint64_t newCurrentTask = 0;
                if (qemu.QueryKernelInfo(0, newSwapperPGD, newCurrentTask)) {
                    std::cout << "Got fresh swapper PGD from QMP: 0x" << std::hex << newSwapperPGD
                              << " (was 0x" << kernelInfo.swapper_pgd << ")" << std::dec << std::endl;

                    if (newSwapperPGD != kernelInfo.swapper_pgd) {
                        std::cout << "Swapper PGD changed! Retrying discovery with new value..." << std::endl;
                        kernelInfo.swapper_pgd = newSwapperPGD;
                        kernelInfo.current_task = newCurrentTask;
                        SaveSwapperPGDCache();

                        // Retry the entire extraction phase with new swapper PGD
                        ExtractProcessPGDs();
                        return;  // ExtractProcessPGDs will print its own summary
                    } else {
                        std::cerr << "Swapper PGD unchanged - problem is elsewhere" << std::endl;
                    }
                } else {
                    std::cerr << "Failed to query QMP for fresh swapper PGD" << std::endl;
                }
            } else {
                std::cerr << "QMP not available - cannot refresh swapper PGD" << std::endl;
            }
            std::cerr << "=== Auto-recovery failed - proceeding with incomplete data ===" << std::endl;
        }
    }

private:
    std::string memoryFile;
    std::string kernelProfilePath;
    int memFd;
    void* memBase;
    size_t memorySize;

    KernelInfo kernelInfo;
    std::vector<ProcessInfo> processes;

    // Hybrid refresh optimization
    std::set<uint64_t> suspectLocations;  // Physical addresses where we found task_structs
    bool hasInitialScan = false;          // Whether we've done the initial full scan

    void Cleanup() {
        if (memBase && memBase != MAP_FAILED) {
            munmap(memBase, memorySize);
        }
        if (memFd >= 0) {
            close(memFd);
        }
    }

    struct SwapperCandidate {
        uint64_t pa;
        int score;
        int userEntries;
        int kernelEntries;
        std::string reasons;
    };

    uint64_t FindSwapperPGD() {
        std::cout << "Searching for swapper PGD with improved scoring..." << std::endl;

        std::vector<SwapperCandidate> candidates;

        // Scan ranges where swapper is typically found
        const std::vector<std::pair<uint64_t, uint64_t>> ranges = {
            {0xf0000000, 0x100000000},    // 3.75-4GB
            {0x130000000, 0x140000000},   // 4.75-5GB (highmem)
            {0x70000000, 0x80000000}      // 1.75-2GB (highmem=off)
        };

        for (const auto& [start, end] : ranges) {
            if (start >= memorySize || end > memorySize) continue;

            for (uint64_t offset = start; offset < std::min(end, (uint64_t)memorySize); offset += 0x1000) {
                if (offset + 0x1000 > memorySize) continue;

                // Quick pre-filter
                uint64_t first = *(uint64_t*)((uint8_t*)memBase + offset);
                if (first == 0 || (first & 3) == 0) continue;

                SwapperCandidate cand = AnalyzeSwapperCandidate(offset);
                if (cand.score > 0) {
                    candidates.push_back(cand);
                }
            }
        }

        if (candidates.empty()) {
            std::cout << "No swapper candidates found, returning hardcoded value" << std::endl;
            return 0x136deb000;  // Fallback
        }

        // Sort by score
        std::sort(candidates.begin(), candidates.end(),
                  [](const SwapperCandidate& a, const SwapperCandidate& b) {
                      return a.score > b.score;
                  });

        std::cout << "Top swapper candidates (by heuristic score):" << std::endl;
        for (int i = 0; i < std::min(5, (int)candidates.size()); i++) {
            const auto& c = candidates[i];
            std::cout << "  " << (i+1) << ". PA 0x" << std::hex << c.pa
                      << " - Score: " << std::dec << c.score
                      << ", User: " << c.userEntries
                      << ", Kernel: " << c.kernelEntries
                      << " (" << c.reasons << ")" << std::endl;
        }

        // COMPARATIVE TESTING: Test top candidates to see which actually works best
        std::cout << "\nFunctional testing top candidates against discovered processes..." << std::endl;

        int topN = std::min(10, (int)candidates.size());
        uint64_t bestPGD = 0;
        int bestSuccessCount = -1;

        for (int i = 0; i < topN; i++) {
            uint64_t candidatePGD = candidates[i].pa;
            int successCount = 0;
            int totalTested = 0;

            // Test this candidate against all discovered user processes
            for (const auto& proc : processes) {
                if (proc.is_kernel_thread) continue;  // Skip kernel threads
                if (proc.mm_addr == 0) continue;

                totalTested++;

                // Try to translate mm_struct VA to PA using this candidate
                uint64_t mmPA = TranslateVA(proc.mm_addr, candidatePGD);

                // Validate the translation produced a valid mm_struct
                if (mmPA && ValidateMMStruct(mmPA)) {
                    successCount++;
                }

                // Only test first 20 processes (enough for statistical significance)
                if (totalTested >= 20) break;
            }

            if (totalTested > 0) {
                int successRate = (successCount * 100) / totalTested;
                std::cout << "  Candidate " << (i+1) << " (0x" << std::hex << candidatePGD
                          << std::dec << "): " << successCount << "/" << totalTested
                          << " (" << successRate << "%)" << std::endl;

                if (successCount > bestSuccessCount) {
                    bestSuccessCount = successCount;
                    bestPGD = candidatePGD;
                }
            }
        }

        if (bestPGD != 0 && bestSuccessCount > 0) {
            std::cout << "\nSelected swapper PGD via functional testing: 0x" << std::hex
                      << bestPGD << std::dec
                      << " (" << bestSuccessCount << " successful translations)" << std::endl;
            return bestPGD;
        }

        // Fallback to heuristic score if functional testing inconclusive
        std::cout << "\nFunctional testing inconclusive, using heuristic score..." << std::endl;
        uint64_t best = candidates[0].pa;
        std::cout << "Selected swapper PGD: 0x" << std::hex << best << std::dec << std::endl;
        return best;
    }

    SwapperCandidate AnalyzeSwapperCandidate(uint64_t offset) {
        SwapperCandidate result;
        result.pa = offset + 0x40000000;
        result.score = 0;
        result.userEntries = 0;
        result.kernelEntries = 0;
        result.reasons = "";

        // Count user and kernel entries
        for (int i = 0; i < 512; i++) {
            uint64_t entryOffset = offset + (i * 8);
            if (entryOffset + 8 > memorySize) break;

            uint64_t entry = *(uint64_t*)((uint8_t*)memBase + entryOffset);
            if (entry == 0) continue;

            uint32_t entryType = entry & 3;
            if (entryType != 1 && entryType != 3) continue;

            if (i < 256) {
                result.userEntries++;
            } else {
                result.kernelEntries++;
            }
        }

        // CRITICAL: Swapper has VERY FEW user entries (typically just 1)
        if (result.userEntries == 1) {
            result.score += 100;  // Huge weight for single user entry
            result.reasons += "Single user entry (swapper signature!)";
        } else if (result.userEntries == 2) {
            result.score += 50;
            result.reasons += "Two user entries";
        } else if (result.userEntries <= 4) {
            result.score += 20;
            result.reasons += std::to_string(result.userEntries) + " user entries";
        } else if (result.userEntries <= 16) {
            result.score += 5;
            result.reasons += std::to_string(result.userEntries) + " user entries";
        } else {
            // Many user entries = likely a process PGD
            result.score -= 50;
            result.reasons += "Too many user entries!";
        }

        // Check PGD[256] for kernel text mapping
        uint64_t pgd256 = *(uint64_t*)((uint8_t*)memBase + offset + (256 * 8));
        if (pgd256 != 0 && (pgd256 & 3) != 0) {
            result.score += 15;
            if (!result.reasons.empty()) result.reasons += ", ";
            result.reasons += "Has PGD[256]";
        } else {
            result.score -= 20;
            if (!result.reasons.empty()) result.reasons += ", ";
            result.reasons += "Missing PGD[256]!";
        }

        // Kernel entries - swapper should have some but not all
        if (result.kernelEntries >= 2 && result.kernelEntries <= 20) {
            result.score += 20;
            if (!result.reasons.empty()) result.reasons += ", ";
            result.reasons += std::to_string(result.kernelEntries) + " kernel entries";
        } else if (result.kernelEntries > 100) {
            result.score -= 10;
            if (!result.reasons.empty()) result.reasons += ", ";
            result.reasons += "Suspicious kernel count";
        }

        return result;
    }

    bool IsKernelPointer(uint64_t ptr) {
        // Kernel pointers have top 16 bits = 0xFFFF
        return (ptr >> 48) == 0xFFFF;
    }

    bool ValidateMMStruct(uint64_t mmPA) {
        // Check if physical address is in valid RAM range
        if (mmPA < 0x40000000 || mmPA >= 0x40000000 + memorySize) {
            return false;
        }

        uint64_t mmOffset = mmPA - 0x40000000;

        // Check we have enough space to read mm_struct fields
        if (mmOffset + 0x80 > memorySize) {
            return false;
        }

        // Read mm_users at offset 0x74 - should be positive and reasonable
        uint32_t mm_users = *(uint32_t*)((uint8_t*)memBase + mmOffset + 0x74);
        if (mm_users == 0 || mm_users > 10000) {
            return false;  // 0 means being torn down, >10000 is unrealistic
        }

        // Read PGD at offset 0x68 - should be a kernel pointer
        uint64_t pgd = *(uint64_t*)((uint8_t*)memBase + mmOffset + 0x68);
        if (!IsKernelPointer(pgd)) {
            return false;
        }

        // Read maple tree root at offset 0x40 + 0x8
        uint64_t mtRoot = *(uint64_t*)((uint8_t*)memBase + mmOffset + 0x40 + 0x8);
        if (mtRoot != 0 && !IsKernelPointer(mtRoot)) {
            return false;  // If present, should be kernel pointer
        }

        return true;
    }

    bool ValidateLinkedList(uint8_t* task) {
        uint64_t* nextPtr = (uint64_t*)(task + kernelInfo.offsets.tasks_list);
        uint64_t* prevPtr = (uint64_t*)(task + kernelInfo.offsets.tasks_list + 8);

        if (*nextPtr == 0 || *prevPtr == 0) return false;
        if (!IsKernelPointer(*nextPtr) || !IsKernelPointer(*prevPtr)) return false;

        return true;
    }

    int CountKernelPointers(uint8_t* task) {
        int count = 0;
        // Check first 512 bytes for kernel pointers
        for (uint64_t checkOffset = 0; checkOffset < 512; checkOffset += 8) {
            uint64_t* ptr = (uint64_t*)(task + checkOffset);
            if (IsKernelPointer(*ptr)) {
                count++;
                if (count >= 10) break;
            }
        }
        return count;
    }

    int CountCaseTransitions(const std::string& str) {
        int transitions = 0;
        for (size_t i = 1; i < str.length(); i++) {
            bool prevIsUpper = (str[i-1] >= 'A' && str[i-1] <= 'Z');
            bool currIsUpper = (str[i] >= 'A' && str[i] <= 'Z');
            bool prevIsLower = (str[i-1] >= 'a' && str[i-1] <= 'z');
            bool currIsLower = (str[i] >= 'a' && str[i] <= 'z');

            if ((prevIsUpper && currIsLower) || (prevIsLower && currIsUpper)) {
                transitions++;
            }
        }
        return transitions;
    }

    bool IsPrintableString(const std::string& str) {
        // Exact implementation from web version
        if (str.length() < 2 || str.length() > 15) return false;

        int alphaNum = 0;
        int special = 0;
        int invalid = 0;

        for (char c : str) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                alphaNum++;
            } else if (c == '/' || c == '-' || c == '_' || c == ':' || c == '.' || c == '[' || c == ']') {
                special++;
            } else if (c < ' ' || c > '~') {
                invalid++;
            }
        }

        // Must have mostly alphanumeric characters
        return alphaNum >= 2 && invalid == 0 && (alphaNum + special) >= str.length() * 0.8;
    }

    bool CheckTaskStruct(uint64_t offset, ProcessInfo& info) {
        if (offset + 9088 >= memorySize) return false;  // TASK_STRUCT_SIZE

        uint8_t* task = (uint8_t*)memBase + offset;

        // Read potential PID
        uint32_t pid = *(uint32_t*)(task + kernelInfo.offsets.pid);

        // Basic PID validation
        if (pid == 0 || pid > 32768) return false;  // PID 0 is swapper, max is typically 32768

        // Read comm (process name) - match web version exactly
        char* comm = (char*)(task + kernelInfo.offsets.comm);

        // Find null terminator (web version uses indexOf(0))
        int nullIdx = -1;
        for (int i = 0; i < 16; i++) {
            if (comm[i] == 0) {
                nullIdx = i;
                break;
            }
        }

        // Web version: returns null if nullIdx === 0 || nullIdx > 15
        // Note: nullIdx == -1 means no null found, which should fail the > 15 check
        // But in JS, indexOf returns -1 which is NOT > 15, so this should pass
        // Actually the web version uses Uint8Array.indexOf() which returns -1 if not found
        // And -1 is NOT > 15, so only nullIdx === 0 would fail here for empty string
        if (nullIdx == 0) {
            return false;  // Empty string
        }
        if (nullIdx == -1) {
            return false;  // No null terminator in 16 bytes
        }

        // Check ALL characters are printable ASCII (0x20-0x7E)
        // Web version uses regex: /^[\x20-\x7E]+$/
        for (int i = 0; i < nullIdx; i++) {
            if (comm[i] < 0x20 || comm[i] > 0x7E) {
                return false;  // Non-printable character
            }
        }

        std::string name(comm, nullIdx);

        // Check if it's a known process
        bool isKnown = false;
        for (const auto& known : KNOWN_PROCESSES) {
            if (name.find(known) != std::string::npos) {
                isKnown = true;
                break;
            }
        }

        // Check if name is valid - be ULTRA strict (from web version)
        if (!IsPrintableString(name)) {
            return false;
        }

        // Reject very short names unless known
        if (name.length() < 3 && !isKnown) {
            return false;
        }

        // Must match pattern: ^[a-zA-Z\/][a-zA-Z0-9\-_\/\[\]:\.\$\s~]*$
        // Added space and tilde for Firefox process names like "Utility Process" and "AudioIP~allback"
        if (name.length() > 0) {
            char first = name[0];
            if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '/')) {
                return false;
            }
            for (size_t i = 1; i < name.length(); i++) {
                char c = name[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '/' || c == '[' || c == ']' || c == ':' ||
                      c == '.' || c == '$' || c == ' ' || c == '~')) {
                    return false;
                }
            }
        }

        // Require at least 2 alphanumeric characters
        int alphaCount = 0;
        for (char c : name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                alphaCount++;
            }
        }
        if (alphaCount < 2) {
            return false;
        }

        // Check case transitions for mixed-case names
        if (!isKnown && name.length() > 2) {
            int upperCount = 0, lowerCount = 0;
            for (char c : name) {
                if (c >= 'A' && c <= 'Z') upperCount++;
                if (c >= 'a' && c <= 'z') lowerCount++;
            }

            if (upperCount > 0 && lowerCount > 0) {
                int transitions = CountCaseTransitions(name);
                if (transitions > name.length() / 2) {
                    return false;  // Too many case transitions
                }
            }
        }

        // Check kernel pointer count FIRST - web version requires at least 3
        int kernelPtrCount = CountKernelPointers(task);
        if (kernelPtrCount < 3) {
            return false;  // Web version early rejects if < 3 kernel pointers
        }

        // Calculate validity score
        int validityScore = 0;

        if (isKnown) validityScore += 3;

        bool hasValidList = ValidateLinkedList(task);
        if (hasValidList) validityScore += 2;

        if (kernelPtrCount >= 5) validityScore += 2;
        if (kernelPtrCount >= 10) validityScore += 1;

        // Check mm pointer
        uint64_t mmPtr = *(uint64_t*)(task + kernelInfo.offsets.mm);
        if (mmPtr == 0 || IsKernelPointer(mmPtr)) {
            validityScore += 1;
        }

        // Require score >= 3
        if (validityScore < 3) {
            return false;
        }

        // Additional mm_struct validation
        if (mmPtr != 0) {
            // For user processes, mm should be kernel VA or in guest RAM range
            if (!IsKernelPointer(mmPtr)) {
                // Check if it's in guest RAM range (0x40000000 to 0x1C0000000 for 6GB)
                // Physical addresses in guest RAM would be 0x40000000 to 0x1C0000000
                if (mmPtr < 0x40000000 || mmPtr >= 0x1C0000000) {
                    return false;  // Neither kernel VA nor plausible physical address
                }
            }
        }

        // Check tasks list pointers look valid (should be kernel addresses)
        uint64_t tasksNext = *(uint64_t*)(task + kernelInfo.offsets.tasks_next);
        uint64_t tasksPrev = *(uint64_t*)(task + kernelInfo.offsets.tasks_prev);

        // Tasks pointers are now validated in ValidateLinkedList

        // If we get here, it's likely a valid task_struct
        // Store as physical address (file offset + GUEST_RAM_START)
        info.task_addr = offset + 0x40000000;  // Convert to physical address
        info.pid = pid;

        // Process name already extracted and validated above
        info.comm = name;

        // mm_struct pointer was already read earlier for validation

        // Store mm_struct info
        info.mm_addr = mmPtr;  // Store the kernel VA (or 0 for kernel threads)
        info.has_mm = (mmPtr != 0);
        info.is_kernel_thread = (mmPtr == 0);
        info.pgd = 0;  // Would require VA->PA translation to read from mm_struct

        return true;
    }

    uint64_t GetNextTask(uint64_t currentTaskPA) {
        if (currentTaskPA >= memorySize) return 0;

        uint8_t* task = (uint8_t*)memBase + currentTaskPA;
        uint64_t nextPtr = *(uint64_t*)(task + kernelInfo.offsets.tasks_next);

        // The tasks list pointer points to the tasks field of the next task_struct
        // We need to subtract the offset to get the actual task_struct address
        if (nextPtr > kernelInfo.offsets.tasks_next) {
            return nextPtr - kernelInfo.offsets.tasks_next;
        }

        return 0;
    }

    bool WalkProcessListFromCurrentTask() {
        if (kernelInfo.current_task == 0) return false;

        processes.clear();
        std::set<uint64_t> visitedTasks;

        // current_task is a kernel VA, translate to PA
        uint64_t currentTaskPA = TranslateVA(kernelInfo.current_task, kernelInfo.swapper_pgd);
        if (currentTaskPA == 0) {
            std::cerr << "Failed to translate current_task VA to PA" << std::endl;
            return false;
        }

        // Offset to file position
        if (currentTaskPA < 0x40000000) {
            std::cerr << "current_task PA out of range: 0x" << std::hex << currentTaskPA << std::dec << std::endl;
            return false;
        }

        uint64_t startOffset = currentTaskPA - 0x40000000;
        uint64_t currentOffset = startOffset;
        int processCount = 0;
        const int MAX_PROCESSES = 10000;  // Safety limit

        std::cout << "Starting walk from PA 0x" << std::hex << currentTaskPA << std::dec << std::endl;

        do {
            // Avoid infinite loops
            if (processCount++ > MAX_PROCESSES) {
                std::cerr << "Too many processes, stopping walk" << std::endl;
                break;
            }

            // Check if we've seen this task before
            if (visitedTasks.count(currentOffset) > 0) {
                // We've completed the circular list
                std::cout << "Completed circular list walk" << std::endl;
                break;
            }
            visitedTasks.insert(currentOffset);

            // Read task_struct at current offset
            if (currentOffset >= memorySize) {
                std::cerr << "Task offset out of bounds" << std::endl;
                break;
            }

            uint8_t* task = (uint8_t*)memBase + currentOffset;

            // Extract process info
            ProcessInfo proc;
            proc.task_addr = currentOffset + 0x40000000;  // Convert back to PA
            proc.pid = *(uint32_t*)(task + kernelInfo.offsets.pid);

            // Validate PID
            if (proc.pid == 0 || proc.pid > 1000000) {
                std::cerr << "Invalid PID " << proc.pid << " at offset 0x"
                          << std::hex << currentOffset << std::dec << std::endl;
                break;
            }

            // Extract comm (process name)
            char* comm = (char*)(task + kernelInfo.offsets.comm);
            proc.comm = std::string(comm, strnlen(comm, 16));

            // Get mm_struct pointer
            proc.mm_addr = *(uint64_t*)(task + kernelInfo.offsets.mm);
            proc.has_mm = (proc.mm_addr != 0);
            proc.is_kernel_thread = !proc.has_mm;

            processes.push_back(proc);

            // Get next task
            uint64_t nextVA = *(uint64_t*)(task + kernelInfo.offsets.tasks_next);

            // The next pointer points to the tasks member of the next task_struct
            // Subtract offset to get actual task_struct address
            if (nextVA <= kernelInfo.offsets.tasks_next) {
                std::cerr << "Invalid next pointer" << std::endl;
                break;
            }

            uint64_t nextTaskVA = nextVA - kernelInfo.offsets.tasks_next;

            // Translate to PA
            uint64_t nextTaskPA = TranslateVA(nextTaskVA, kernelInfo.swapper_pgd);
            if (nextTaskPA == 0) {
                std::cerr << "Failed to translate next task VA" << std::endl;
                break;
            }

            if (nextTaskPA < 0x40000000) {
                std::cerr << "Next task PA out of range" << std::endl;
                break;
            }

            currentOffset = nextTaskPA - 0x40000000;

            // Check if we're back at the start
            if (currentOffset == startOffset) {
                std::cout << "Completed circular list" << std::endl;
                break;
            }

        } while (true);

        std::cout << "Walk found " << processes.size() << " processes" << std::endl;
        return processes.size() > 0;
    }

    // Page table walking functions (from web version)
    uint64_t TranslateVA(uint64_t va, uint64_t pgdBase) {
        // ARM64 page table translation (4KB pages, 48-bit VA)
        // Level 0 (PGD): bits 47:39
        // Level 1 (PUD): bits 38:30
        // Level 2 (PMD): bits 29:21
        // Level 3 (PTE): bits 20:12
        // Offset: bits 11:0

        const uint64_t VALID_BIT = 1;
        const uint64_t TABLE_BIT = 2;  // For non-leaf entries
        const uint64_t PA_MASK = 0x0000FFFFFFFFF000ULL;

        // Extract indices
        uint32_t pgdIndex = (va >> 39) & 0x1FF;
        uint32_t pudIndex = (va >> 30) & 0x1FF;
        uint32_t pmdIndex = (va >> 21) & 0x1FF;
        uint32_t pteIndex = (va >> 12) & 0x1FF;
        uint32_t pageOffset = va & 0xFFF;

        // Read PGD entry
        // pgdBase should be a physical address in guest RAM
        if (pgdBase < 0x40000000 || pgdBase >= 0x40000000 + memorySize) {
            // Invalid PGD base address
            return 0;
        }
        uint64_t pgdOffset = (pgdBase - 0x40000000) + (pgdIndex * 8);
        if (pgdOffset + 8 > memorySize) return 0;
        uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);

        if (!(pgdEntry & VALID_BIT)) return 0;
        if (!(pgdEntry & TABLE_BIT)) return 0;  // Not a table descriptor

        // Read PUD entry
        uint64_t pudBase = pgdEntry & PA_MASK;
        uint64_t pudOffset = (pudBase - 0x40000000) + (pudIndex * 8);
        if (pudOffset + 8 > memorySize) return 0;
        uint64_t pudEntry = *(uint64_t*)((uint8_t*)memBase + pudOffset);

        if (!(pudEntry & VALID_BIT)) return 0;

        // Check if PUD is a huge page (1GB)
        if (!(pudEntry & TABLE_BIT)) {
            // 1GB page
            return (pudEntry & 0x0000FFFFC0000000ULL) | (va & 0x3FFFFFFF);
        }

        // Read PMD entry
        uint64_t pmdBase = pudEntry & PA_MASK;
        uint64_t pmdOffset = (pmdBase - 0x40000000) + (pmdIndex * 8);
        if (pmdOffset + 8 > memorySize) return 0;
        uint64_t pmdEntry = *(uint64_t*)((uint8_t*)memBase + pmdOffset);

        if (!(pmdEntry & VALID_BIT)) return 0;

        // Check if PMD is a huge page (2MB)
        if (!(pmdEntry & TABLE_BIT)) {
            // 2MB page
            return (pmdEntry & 0x0000FFFFFFE00000ULL) | (va & 0x1FFFFF);
        }

        // Read PTE entry
        uint64_t pteBase = pmdEntry & PA_MASK;
        uint64_t pteOffset = (pteBase - 0x40000000) + (pteIndex * 8);
        if (pteOffset + 8 > memorySize) return 0;
        uint64_t pteEntry = *(uint64_t*)((uint8_t*)memBase + pteOffset);

        if (!(pteEntry & VALID_BIT)) return 0;

        // 4KB page
        return (pteEntry & PA_MASK) | pageOffset;
    }

    // Walk maple tree to get memory sections (VMAs)
    bool WalkMapleTree(uint64_t mmPA, std::vector<MemorySection>& sections) {
        // Read maple tree root at mm_struct + 0x40
        uint64_t mtOffset = mmPA - 0x40000000 + 0x40;
        if (mtOffset + 0x10 > memorySize) {
            std::cout << "[MapleTree] mtOffset out of bounds: " << std::hex << mtOffset << std::dec << "\n" << std::flush;
            return false;
        }

        // The actual root node is at offset 0x8 within maple_tree
        uint64_t rootPtrVA = *(uint64_t*)((uint8_t*)memBase + mtOffset + 0x8);
        if (!rootPtrVA || !IsKernelPointer(rootPtrVA)) {
            std::cout << "[MapleTree] Invalid root pointer: " << std::hex << rootPtrVA << std::dec << "\n" << std::flush;
            return false;
        }

        std::cout << "[MapleTree] Walking from root: " << std::hex << rootPtrVA << std::dec << "\n" << std::flush;
        // Walk the maple tree starting from root
        WalkMapleNode(rootPtrVA, sections, 0);
        std::cout << "[MapleTree] Found " << sections.size() << " sections\n" << std::flush;
        return !sections.empty();
    }

    void WalkMapleNode(uint64_t nodePtr, std::vector<MemorySection>& sections, int depth) {
        if (!nodePtr || nodePtr == 0 || depth > 15) {
            return; // Prevent infinite recursion
        }

        // Maple nodes have type encoding in low 8 bits (MAPLE_NODE_MASK = 0xFF)
        // The kernel uses mte_to_node() to clean pointers
        const uint64_t MAPLE_NODE_MASK = 0xFF;
        uint32_t nodeType = nodePtr & MAPLE_NODE_MASK;
        uint64_t cleanNodePtr = nodePtr & ~MAPLE_NODE_MASK;

        // Extract maple_type using kernel's exact method:
        // mte_node_type() = (entry >> MAPLE_NODE_TYPE_SHIFT) & MAPLE_NODE_TYPE_MASK
        const int MAPLE_NODE_TYPE_SHIFT = 3;
        const int MAPLE_NODE_TYPE_MASK = 0x0F;
        uint32_t mapleType = (nodePtr >> MAPLE_NODE_TYPE_SHIFT) & MAPLE_NODE_TYPE_MASK;

        // enum maple_type values from kernel
        const uint32_t MAPLE_DENSE = 0;
        const uint32_t MAPLE_LEAF_64 = 1;
        const uint32_t MAPLE_RANGE_64 = 2;
        const uint32_t MAPLE_ARANGE_64 = 3;

        // THE CRITICAL TEST from kernel: ma_is_leaf(type) = type < maple_range_64
        bool isLeafNode = mapleType < MAPLE_RANGE_64;  // type 0 or 1 = leaf
        bool isInternalNode = !isLeafNode;              // type 2 or 3 = internal

        // Translate kernel VA to physical address
        uint64_t actualNodePA;
        if (IsKernelPointer(cleanNodePtr)) {
            actualNodePA = TranslateVA(cleanNodePtr, kernelInfo.swapper_pgd);
            if (!actualNodePA) {
                return; // Can't translate
            }
        } else {
            actualNodePA = cleanNodePtr;
        }

        if (actualNodePA < 0x40000000 || actualNodePA >= 0x40000000 + memorySize) {
            return; // Invalid physical address
        }

        uint64_t nodeOffset = actualNodePA - 0x40000000;

        // CRITICAL DISTINCTION: Internal nodes vs Leaf nodes
        // Internal nodes have slots pointing to child nodes
        // Leaf nodes have slots pointing to actual data (vm_area_structs)

        int numSlots;
        int slotsOffset;
        int metadataOffset;

        if (mapleType == MAPLE_ARANGE_64) {
            // maple_arange_64 - ALWAYS an internal node!
            numSlots = 10;
            slotsOffset = 80;
            metadataOffset = 240;
        } else if (mapleType == MAPLE_RANGE_64) {
            // maple_range_64 - ALWAYS an internal node!
            numSlots = 16;
            slotsOffset = 128;
            metadataOffset = 256;
        } else if (isLeafNode) {
            // Leaf node - contains BOTH pivots (keys) and values (vm_area_struct pointers)
            if (mapleType == MAPLE_DENSE) {
                // maple_dense stores values inline
                numSlots = 15;
                slotsOffset = 8;  // Values start right after header
                metadataOffset = 248;
            } else {
                // maple_leaf_64 has complex structure:
                // [0x00-0x7F]: Pivots (16 x 8 bytes = 128 bytes)
                // [0x80-0xFF]: Slots (16 x 8 bytes = 128 bytes)
                // [0x100+]: Metadata
                numSlots = 16;
                slotsOffset = 128;  // Skip pivot array, start at slots
                metadataOffset = 256;
            }
        } else {
            // Unknown type - try to handle as potential data node
            numSlots = 16;
            slotsOffset = 128;
            metadataOffset = 256;
        }

        // Read metadata to get actual slot count
        if (nodeOffset + metadataOffset + 16 <= memorySize) {
            uint8_t* metadata = (uint8_t*)memBase + nodeOffset + metadataOffset;
            // For leaf nodes, metadata[0] is often max_index
            if (metadata[0] > 0 && metadata[0] < numSlots) {
                numSlots = metadata[0] + 1;
            }
        }

        // Process slots based on whether this is an internal or leaf node
        for (int i = 0; i < numSlots; i++) {
            if (nodeOffset + slotsOffset + (i + 1) * 8 > memorySize) break;

            uint64_t slotPtr = *(uint64_t*)((uint8_t*)memBase + nodeOffset + slotsOffset + i * 8);
            if (!slotPtr || slotPtr == 0) {
                continue;
            }

            // For maple dense nodes, check if we're looking at key-value pairs
            if (isLeafNode && mapleType == MAPLE_DENSE) {
                if (i % 2 == 0 && i + 1 < numSlots) {
                    // This is a key (address range), next slot is the value (VMA pointer)
                    uint64_t nextSlot = *(uint64_t*)((uint8_t*)memBase + nodeOffset + slotsOffset + (i + 1) * 8);
                    if (nextSlot && IsKernelPointer(nextSlot)) {
                        // Try to extract VMA from the value (odd slot)
                        uint64_t translated = TranslateVA(nextSlot, kernelInfo.swapper_pgd);
                        if (translated) {
                            ExtractVMA(translated, sections);
                        }
                        continue; // Skip the next slot since we processed it
                    }
                }
            }

            // Skip small values that are likely metadata/indices
            if (slotPtr < 0x1000) {
                continue;
            }

            if (isInternalNode) {
                // INTERNAL NODE: Slots contain pointers to child nodes
                if (IsKernelPointer(slotPtr)) {
                    // This looks like a kernel VA - follow it as a child node
                    WalkMapleNode(slotPtr, sections, depth + 1);
                }
            } else if (isLeafNode) {
                // LEAF NODE: Slots contain actual data (vm_area_structs)
                if (IsKernelPointer(slotPtr)) {
                    // Try to read as vm_area_struct
                    uint64_t translated = TranslateVA(slotPtr, kernelInfo.swapper_pgd);
                    if (translated) {
                        ExtractVMA(translated, sections);
                    }
                }
            }
        }
    }

    void ExtractVMA(uint64_t vmaPA, std::vector<MemorySection>& sections) {
        if (vmaPA < 0x40000000 || vmaPA >= 0x40000000 + memorySize) return;

        uint64_t vmaOffset = vmaPA - 0x40000000;
        if (vmaOffset + 0x100 > memorySize) return;

        uint8_t* vma = (uint8_t*)memBase + vmaOffset;

        // Read VMA fields at correct offsets
        uint64_t vmStart = *(uint64_t*)(vma + 0x00);  // vm_start at offset 0
        uint64_t vmEnd = *(uint64_t*)(vma + 0x08);    // vm_end at offset 8
        uint64_t vmFlags = *(uint64_t*)(vma + 0x20);  // vm_flags at offset 0x20 (32)
        uint64_t vmFile = *(uint64_t*)(vma + 0x80);   // vm_file at offset 0x80 (128)

        // Validate it looks like a VMA (user addresses)
        // ARM64 user space can go up to ~0xffffff000000 (48-bit addresses)
        if (vmStart && vmEnd && vmEnd > vmStart &&
            vmStart >= 0x10000 && vmStart < 0x1000000000000ULL &&
            vmEnd >= 0x10000 && vmEnd < 0x1000000000000ULL &&
            (vmEnd - vmStart) >= 0x1000) {  // At least one page

            MemorySection section;
            section.start = vmStart;
            section.end = vmEnd;
            section.flags = vmFlags;
            section.name = "";

            // Try to extract filename if vm_file exists
            if (vmFile && IsKernelPointer(vmFile)) {
                ExtractFileName(vmFile, section.name);
            }

            // Classify the section based on flags and filename
            section.type = ClassifySection(vmStart, vmEnd, vmFlags, section.name);

            sections.push_back(section);
        }
    }

    // VM flag constants (from Linux kernel)
    static constexpr uint64_t VM_READ    = 0x00000001;
    static constexpr uint64_t VM_WRITE   = 0x00000002;
    static constexpr uint64_t VM_EXEC    = 0x00000004;
    static constexpr uint64_t VM_SHARED  = 0x00000008;
    static constexpr uint64_t VM_GROWSDOWN = 0x00000100;

    MemorySection::Type ClassifySection(uint64_t start, uint64_t end, uint64_t flags, const std::string& filename) {
        // Check for special kernel-provided mappings by filename
        if (!filename.empty()) {
            if (filename == "[vdso]") return MemorySection::VDSO;
            if (filename == "[vvar]") return MemorySection::VVAR;
            if (filename == "[stack]" || filename.find("[stack:") == 0) return MemorySection::STACK;
            if (filename == "[heap]") return MemorySection::HEAP;

            // Shared libraries (.so files)
            if (filename.find(".so") != std::string::npos) {
                return MemorySection::SHARED_LIB;
            }

            // Executable code (has exec permission and is a file)
            if (flags & VM_EXEC) {
                return MemorySection::EXECUTABLE;
            }

            // Regular file-backed mapping
            return MemorySection::FILE_BACKED;
        }

        // Anonymous mappings (no filename)
        if (flags & VM_GROWSDOWN) {
            // Stack grows downward - VM_GROWSDOWN is the definitive stack indicator
            return MemorySection::STACK;
        }

        // Writable, non-executable regions are likely heap
        if ((flags & VM_WRITE) && !(flags & VM_EXEC)) {
            return MemorySection::HEAP;
        }

        // Read-only anonymous mappings
        if (!(flags & VM_WRITE) && !(flags & VM_EXEC)) {
            return MemorySection::ANONYMOUS;  // Could be guard pages, etc.
        }

        // Everything else anonymous
        return MemorySection::ANONYMOUS;
    }

    void ExtractFileName(uint64_t vmFile, std::string& filename) {
        // vm_file points to a struct file
        uint64_t filePA = TranslateVA(vmFile, kernelInfo.swapper_pgd);
        if (!filePA) return;

        uint64_t fileOffset = filePA - 0x40000000;
        if (fileOffset + 0x100 > memorySize) return;

        // struct file: f_path at offset 0x40 (64 decimal)
        // struct path contains: dentry at +8
        // So dentry is at file offset 0x48
        uint64_t dentry = *(uint64_t*)((uint8_t*)memBase + fileOffset + 0x48);

        if (dentry && IsKernelPointer(dentry)) {
            uint64_t dentryPA = TranslateVA(dentry, kernelInfo.swapper_pgd);
            if (!dentryPA) return;

            uint64_t dentryOffset = dentryPA - 0x40000000;
            if (dentryOffset + 0x100 > memorySize) return;

            // dentry has d_name (qstr) at offset 0x20 (32 decimal)
            // qstr.name pointer is at offset 0x8 within qstr
            uint64_t dNamePtr = *(uint64_t*)((uint8_t*)memBase + dentryOffset + 0x20 + 0x8);

            if (dNamePtr && IsKernelPointer(dNamePtr)) {
                uint64_t namePA = TranslateVA(dNamePtr, kernelInfo.swapper_pgd);
                if (!namePA) return;

                uint64_t nameOffset = namePA - 0x40000000;
                if (nameOffset + 256 > memorySize) return;

                // Read up to 256 bytes for the filename
                char* nameStr = (char*)memBase + nameOffset;
                size_t len = strnlen(nameStr, 256);
                if (len > 0 && len < 256) {
                    filename = std::string(nameStr, len);
                }
            }
        }
    }

}; // End of KernelDiscovery class

} // namespace Haywire

// Standalone test program
// Main function removed - this is now a library class
// Use test_kernel_discovery.cpp for testing