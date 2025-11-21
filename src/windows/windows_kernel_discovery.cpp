/**
 * Windows Kernel Discovery for Haywire
 *
 * Windows-specific implementation of kernel structure discovery
 * Discovers EPROCESS, PEB, and memory sections from Windows kernel
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstring>
#include <chrono>
#include "../../include/platform_compat.h"
#ifdef _WIN32
    #include <io.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #define open _open
    #define close _close
    #define O_RDONLY _O_RDONLY
    // Windows doesn't have pread, need to implement it
    inline ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
        if (_lseek(fd, offset, SEEK_SET) < 0) return -1;
        return _read(fd, buf, static_cast<unsigned int>(count));
    }
#else
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif
#include "../../include/ikernel_discovery.h"
#include "../../include/qemu_connection.h"

// Include profile loader
#include "../kernel_profile_loader.h"

namespace Haywire {

/**
 * Windows-specific kernel discovery implementation
 *
 * Uses EPROCESS, PEB, and VAD tree structures from Windows kernel
 */
class WindowsKernelDiscovery : public IKernelDiscovery {
public:
    using MemorySection = IKernelDiscovery::MemorySection;
    using PTE = IKernelDiscovery::PTE;
    using ProcessInfo = IKernelDiscovery::ProcessInfo;
    using KernelInfo = IKernelDiscovery::KernelInfo;

    WindowsKernelDiscovery(const std::string& memFile = "/tmp/haywire-vm-mem",
                          const std::string& profilePath = "")
        : memoryFile(memFile), profilePath(profilePath) {
        std::cout << "[WindowsKernelDiscovery] Initialized with memory file: " << memFile << std::endl;

        // Load kernel profile
        std::string profileToLoad = profilePath;
        if (profileToLoad.empty()) {
            // Default to Windows 11 Build 26100 profile
            profileToLoad = "profiles/windows/windows-11-26100-x86_64.json";
        }

        if (!KernelProfileLoader::LoadProfile(profileToLoad, profile)) {
            std::cerr << "[WindowsKernelDiscovery] Warning: Failed to load profile, using defaults" << std::endl;
            // Profile struct already has sensible defaults
        }
    }

    ~WindowsKernelDiscovery() {
        Cleanup();
    }

    // Lifecycle
    bool Initialize() override {
        std::cout << "[WindowsKernelDiscovery] Opening memory file..." << std::endl;

        // Open memory file
        memfd = open(memoryFile.c_str(), O_RDONLY);
        if (memfd < 0) {
            std::cerr << "[WindowsKernelDiscovery] Failed to open memory file: "
                     << strerror(errno) << std::endl;
            return false;
        }

        // Get file size
        struct stat st;
        if (fstat(memfd, &st) < 0) {
            std::cerr << "[WindowsKernelDiscovery] Failed to stat memory file" << std::endl;
            close(memfd);
            memfd = -1;
            return false;
        }
        memorySize = st.st_size;

        std::cout << "[WindowsKernelDiscovery] Memory file opened: "
                 << (memorySize / (1024*1024)) << " MB" << std::endl;

        // Initialize QMP and Monitor connections for reading kernel structures
        qmp = &QemuConnection::getInstance();
        if (!qmp->ConnectQMP("localhost", 4445)) {
            std::cerr << "[WindowsKernelDiscovery] Warning: QMP connection failed - "
                     << "kernel structure access will be limited" << std::endl;
        }

        // CRITICAL: Connect to monitor for page table reads (page tables NOT in memory file!)
        if (!qmp->ConnectMonitor("localhost", 4444)) {
            std::cerr << "[WindowsKernelDiscovery] Warning: Monitor connection failed - "
                     << "page table translation will not work!" << std::endl;
            // Don't fail - we can still do limited discovery
        }

        return true;
    }

    void Cleanup() override {
        if (memfd >= 0) {
            close(memfd);
            memfd = -1;
        }
        processes.clear();
    }

    void SetDebugLogging(bool enabled) override {
        // TODO: Implement debug logging for Windows discovery
        (void)enabled;
    }

    // Discovery methods
    bool DiscoverKernel() override {
        std::cout << "[WindowsKernelDiscovery] Discovering Windows kernel structures..." << std::endl;

        // For Windows, we need to find:
        // 1. System process EPROCESS (PID 4)
        // 2. Extract DirectoryTableBase for kernel page tables

        // This will be implemented after we can scan for EPROCESS structures
        std::cout << "[WindowsKernelDiscovery] Kernel discovery not yet fully implemented" << std::endl;

        return true;  // Allow to proceed for now
    }

    bool DiscoverProcesses() override {
        auto startTime = std::chrono::steady_clock::now();

        processes.clear();

        // Step 1: Do blind physical memory scan to find some processes
        std::cout << "[WindowsKernelDiscovery] Scanning physical memory for processes..." << std::endl;
        size_t scanCount = ScanForAllProcesses();
        std::cout << "[WindowsKernelDiscovery] Blind scan found " << scanCount << " process(es)" << std::endl;

        if (scanCount == 0) {
            std::cerr << "[WindowsKernelDiscovery] No processes found via scanning" << std::endl;
            return false;
        }

        // Extract DTBs from the found processes
        // CRITICAL: Store System's DTB for VAD tree walking
        std::vector<uint64_t> knownGoodDTBs;
        for (const auto& proc : processes) {
            uint64_t dtb_addr = proc.task_addr + profile.kprocess_directory_table_base;
            uint8_t dtb_data[8];
            if (ReadPhysicalMemory(dtb_addr, dtb_data, 8)) {
                uint64_t dtb_raw = *reinterpret_cast<uint64_t*>(dtb_data);
                // Mask to get physical address only (bits 12-51)
                uint64_t dtb = dtb_raw & 0x000FFFFFFFFFF000ULL;
                // DTB must be within physical RAM: 0-2GB OR 4-10GB (NOT in PCI hole 2-4GB)
                const uint64_t MAX_PHYSICAL_ADDR = 0x280000000ULL;  // 10GB
                bool dtbInValidRange = (dtb != 0 && dtb < MAX_PHYSICAL_ADDR &&
                                       !(dtb >= 0x80000000ULL && dtb < 0x100000000ULL));  // Not in PCI hole
                if (dtbInValidRange) {
                    knownGoodDTBs.push_back(dtb);
                    std::cout << "[WindowsKernelDiscovery] " << proc.comm << " has DTB=0x"
                              << std::hex << dtb << std::dec << std::endl;

                    // Store System's DTB for kernel VA translation (KPTI workaround)
                    if (proc.pid == 4 && proc.comm == "System") {
                        kernelInfo.swapper_pgd = dtb;
                        kernelInfo.init_task = proc.task_addr;
                        std::cout << "[WindowsKernelDiscovery] ✓ Found System process DTB=0x"
                                  << std::hex << dtb << " at PA 0x" << proc.task_addr
                                  << std::dec << std::endl;
                    }
                }
            }
        }

        std::cout << "[WindowsKernelDiscovery] Found " << knownGoodDTBs.size()
                  << " processes with valid DTBs" << std::endl;

        // Step 2: Find System process candidates
        std::cout << "[WindowsKernelDiscovery] Finding System process..." << std::endl;
        std::vector<uint64_t> systemCandidates = FindAllSystemProcesses();

        if (systemCandidates.empty() || knownGoodDTBs.empty()) {
            std::cout << "[WindowsKernelDiscovery] Cannot walk process list, using scanned processes only" << std::endl;
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            std::cout << "[WindowsKernelDiscovery] Found " << scanCount
                      << " processes in " << duration << "ms" << std::endl;
            return true;  // We have processes from scanning
        }

        std::cout << "[WindowsKernelDiscovery] Found " << systemCandidates.size()
                  << " System candidate(s)" << std::endl;

        // Step 3: Try walking the process list using System's own DTB
        // System process (PID 4) uses the kernel CR3 with full kernel mappings
        std::cout << "[WindowsKernelDiscovery] Attempting to walk ActiveProcessLinks using System's DTB..." << std::endl;

        size_t originalCount = processes.size();
        size_t walkCount = 0;

        // Save the scanned processes in case all walking attempts fail
        std::vector<ProcessInfo> scannedProcs = processes;

        for (size_t i = 0; i < systemCandidates.size() && walkCount == 0; i++) {
            uint64_t systemEprocessPA = systemCandidates[i];

            // Read System's DirectoryTableBase (this is the kernel CR3!)
            uint64_t dtb_addr = systemEprocessPA + profile.kprocess_directory_table_base;
            uint8_t dtb_data[8];
            if (!ReadPhysicalMemory(dtb_addr, dtb_data, 8)) {
                std::cerr << "[WindowsKernelDiscovery] Failed to read DTB from System candidate #" << i << std::endl;
                continue;
            }

            uint64_t systemDTB_raw = *reinterpret_cast<uint64_t*>(dtb_data);

            // Mask to get physical address only (bits 12-51)
            // CR3 has flags/PCID in bits 0-11 and reserved in 52-63
            uint64_t systemDTB = systemDTB_raw & 0x000FFFFFFFFFF000ULL;

            // Validate DTB looks reasonable
            if (systemDTB == 0 || systemDTB >= memorySize) {
                std::cerr << "[WindowsKernelDiscovery] System candidate #" << i
                          << " has invalid DTB=0x" << std::hex << systemDTB
                          << " (raw=0x" << systemDTB_raw << ")" << std::dec << std::endl;
                continue;
            }

            std::cout << "[WindowsKernelDiscovery] Testing System candidate #" << i
                      << " at PA 0x" << std::hex << systemEprocessPA
                      << " with DTB=0x" << systemDTB << std::dec << std::endl;

            // Clear and try walking with this System's DTB
            processes.clear();
            walkCount = WalkProcessListWithTranslation(systemEprocessPA, systemDTB);

            if (walkCount == 0) {
                std::cout << "[WindowsKernelDiscovery] Candidate #" << i << " failed to walk process list" << std::endl;
            } else {
                std::cout << "[WindowsKernelDiscovery] ✓ SUCCESS! Candidate #" << i
                          << " found " << walkCount << " processes via list walking" << std::endl;

                // Store the kernel CR3 for future use
                kernelInfo.swapper_pgd = systemDTB;
                kernelInfo.init_task = systemEprocessPA;
            }
        }

        // If all walking attempts failed, restore scanned processes
        if (walkCount == 0) {
            std::cout << "[WindowsKernelDiscovery] All System candidates failed, using scanned processes only" << std::endl;
            processes = scannedProcs;
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        size_t finalCount = processes.size();
        std::cout << "[WindowsKernelDiscovery] Found " << finalCount
                  << " processes in " << duration << "ms";
        if (walkCount > 0) {
            std::cout << " (via list walking)";
        } else {
            std::cout << " (via scanning only)";
        }
        std::cout << std::endl;

        return finalCount > 0;
    }

    // Access methods
    const std::vector<ProcessInfo>& GetProcesses() const override {
        return processes;
    }

    const KernelInfo& GetKernelInfo() const override {
        return kernelInfo;
    }

    // Per-process operations
    void WalkProcessPageTables(ProcessInfo& proc) override {
        // Extract PTEs for this process
        ExtractPTEsForProcess(proc);
        std::cout << "[WindowsKernelDiscovery] Extracted " << proc.ptes.size() << " PTEs for PID " << proc.pid << std::endl;
    }

    bool ExtractProcessMemoryMap(uint32_t pid) override {
        // Find process by PID
        auto it = std::find_if(processes.begin(), processes.end(),
            [pid](const ProcessInfo& p) { return p.pid == pid; });

        if (it == processes.end()) {
            std::cerr << "[WindowsKernelDiscovery] Process PID " << pid << " not found" << std::endl;
            return false;
        }

        ProcessInfo& proc = *it;
        proc.sections.clear();

        // Read VadRoot from EPROCESS
        uint64_t eprocess_pa = proc.task_addr;
        uint64_t vadroot_va = 0;

        if (!ReadPhysicalMemory(eprocess_pa + profile.eprocess_vad_root,
                               reinterpret_cast<uint8_t*>(&vadroot_va), 8)) {
            std::cerr << "[WindowsKernelDiscovery] Failed to read VadRoot from EPROCESS" << std::endl;
            return false;
        }

        std::cout << "[WindowsKernelDiscovery] PID " << pid << " VadRoot VA: 0x"
                  << std::hex << vadroot_va << std::dec << std::endl;

        // Check if VadRoot is valid
        if ((vadroot_va >> 48) != 0xffff) {
            std::cout << "[WindowsKernelDiscovery] VadRoot is not a kernel VA (empty VAD tree)" << std::endl;
            return false;  // Not kernel VA - likely empty tree
        }

        if (vadroot_va == 0xffffffffffffffff || vadroot_va == 0) {
            std::cout << "[WindowsKernelDiscovery] VadRoot is null/sentinel (empty VAD tree)" << std::endl;
            return false;  // Sentinel/null
        }

        // CRITICAL: Translate VadRoot VA to PA using System's DTB
        // User process DTBs cannot translate kernel VAs due to KPTI
        uint64_t vadroot_pa = TranslateVA(vadroot_va, kernelInfo.swapper_pgd);
        if (vadroot_pa == 0) {
            std::cerr << "[WindowsKernelDiscovery] Failed to translate VadRoot VA to PA" << std::endl;
            std::cerr << "[WindowsKernelDiscovery] Using System DTB: 0x" << std::hex
                      << kernelInfo.swapper_pgd << std::dec << std::endl;
            return false;
        }

        // Walk VAD tree recursively
        WalkVADTree(vadroot_pa, proc.sections);

        // Extract modules from PEB (provides .exe and all DLLs with names)
        ExtractModulesFromPEB(proc, proc.sections);

        // Extract PTEs for each memory section
        if (!proc.sections.empty()) {
            std::cout << "[WindowsKernelDiscovery] Extracting PTEs for " << proc.sections.size() << " sections..." << std::endl;
            ExtractPTEsForProcess(proc);
            std::cout << "[WindowsKernelDiscovery] Extracted " << proc.ptes.size() << " PTEs" << std::endl;
        }

        return !proc.sections.empty();
    }

    void ExtractProcessPGDs() override {
        std::cout << "[WindowsKernelDiscovery] Extracting DirectoryTableBase for each process..." << std::endl;

        for (auto& proc : processes) {
            // DirectoryTableBase is in KPROCESS (first member of EPROCESS)
            uint64_t dtb_addr = proc.task_addr + profile.kprocess_directory_table_base;

            // Read DirectoryTableBase
            uint8_t dtb_data[8];
            if (ReadPhysicalMemory(dtb_addr, dtb_data, 8)) {
                uint64_t dtb_raw = *reinterpret_cast<uint64_t*>(dtb_data);

                // Mask to get physical address only (bits 12-51)
                proc.pgd = dtb_raw & 0x000FFFFFFFFFF000ULL;

                if (proc.pgd != 0) {
                    std::cout << "  PID " << proc.pid << " (" << proc.comm
                             << "): EPROCESS=0x" << std::hex << proc.task_addr
                             << " DTB=0x" << proc.pgd << std::dec << std::endl;
                }
            }
        }
    }

    uint64_t TranslateVA(uint64_t va, uint64_t pgdBase) override {
        // x64 4-level paging: PML4 → PDPT → PD → PT → Physical
        // Each level uses 9 bits of the VA

        uint64_t pml4_index = (va >> 39) & 0x1FF;
        uint64_t pdpt_index = (va >> 30) & 0x1FF;
        uint64_t pd_index = (va >> 21) & 0x1FF;

        // Debug output disabled to avoid flooding console during PTE extraction
        // std::cout << "[TranslateVA] VA=0x" << std::hex << va
        //           << " PGD=0x" << pgdBase
        //           << " PML4_idx=" << pml4_index << std::dec << std::endl;
        uint64_t pt_index = (va >> 12) & 0x1FF;
        uint64_t offset = va & 0xFFF;

        // Read PML4 entry (CRITICAL: Use QMP, not file!)
        uint64_t pml4_addr = (pgdBase & 0x000FFFFFFFFFF000ULL) + (pml4_index * 8);
        uint64_t pml4e;
        if (!ReadPageTableEntry(pml4_addr, reinterpret_cast<uint8_t*>(&pml4e), 8)) {
            std::cerr << " Failed to read PML4 entry via QMP" << std::endl;
            return 0;
        }
        // std::cout << " PML4E=0x" << std::hex << pml4e << std::dec
        //           << " Present=" << (pml4e & 0x1) << std::endl;
        if (!(pml4e & 0x1)) return 0;  // Not present

        // Read PDPT entry - mask to get physical address (bits 12-51)
        uint64_t pdpt_addr = (pml4e & 0x000FFFFFFFFFF000ULL) + (pdpt_index * 8);
        uint64_t pdpte;
        if (!ReadPageTableEntry(pdpt_addr, reinterpret_cast<uint8_t*>(&pdpte), 8)) {
            // std::cerr << " Failed to read PDPT via QMP" << std::endl;
            return 0;
        }
        // std::cout << " PDPTE=0x" << std::hex << pdpte << std::dec << std::endl;

        // Validate PDPTE is not garbage (all 1's is invalid)
        if (pdpte == 0xffffffffffffffff) {
            // std::cerr << " [TranslateVA] PDPTE is invalid (all 1's)" << std::endl;
            return 0;
        }

        // std::cout << " Present=" << (pdpte & 0x1)
        //           << " Huge=" << ((pdpte & 0x80) ? 1 : 0) << std::endl;
        if (!(pdpte & 0x1)) return 0;  // Not present
        if (pdpte & 0x80) {  // 1GB huge page
            uint64_t pa = (pdpte & 0x000FFFFFC0000000ULL) + (va & 0x3FFFFFFF);
            // std::cout << " [TranslateVA] 1GB huge page → PA 0x" << std::hex << pa << std::dec << std::endl;
            return pa;
        }

        // Read PD entry - mask to get physical address (bits 12-51)
        uint64_t pd_addr = (pdpte & 0x000FFFFFFFFFF000ULL) + (pd_index * 8);
        uint64_t pde;
        if (!ReadPageTableEntry(pd_addr, reinterpret_cast<uint8_t*>(&pde), 8)) {
            // std::cerr << " Failed to read PD via QMP" << std::endl;
            return 0;
        }
        // std::cout << " PDE=0x" << std::hex << pde << " Present=" << (pde & 0x1)
        //           << " Huge=" << ((pde & 0x80) ? 1 : 0) << std::dec << std::endl;
        if (!(pde & 0x1)) return 0;  // Not present
        if (pde & 0x80) {  // 2MB huge page
            uint64_t pa = (pde & 0x000FFFFFFFE00000ULL) + (va & 0x1FFFFF);
            // std::cout << " [TranslateVA] 2MB huge page → PA 0x" << std::hex << pa << std::dec << std::endl;
            return pa;
        }

        // Read PT entry - mask to get physical address (bits 12-51)
        uint64_t pt_addr = (pde & 0x000FFFFFFFFFF000ULL) + (pt_index * 8);
        uint64_t pte;
        if (!ReadPageTableEntry(pt_addr, reinterpret_cast<uint8_t*>(&pte), 8)) {
            // std::cerr << " Failed to read PT via QMP" << std::endl;
            return 0;
        }
        // std::cout << " PTE=0x" << std::hex << pte << " Present=" << (pte & 0x1) << std::dec << std::endl;
        if (!(pte & 0x1)) return 0;  // Not present

        // 4KB page - mask to get physical address (bits 12-51)
        uint64_t pa = (pte & 0x000FFFFFFFFFF000ULL) + offset;
        // std::cout << " [TranslateVA] 4KB page → PA 0x" << std::hex << pa << std::dec << std::endl;
        return pa;
    }

    const char* GetOSType() const override { return "Windows"; }
    const char* GetArchitecture() const override { return "x86_64"; }

private:
    std::string memoryFile;
    std::string profilePath;
    int memfd = -1;
    size_t memorySize = 0;
    QemuConnection* qmp = nullptr;

    KernelProfile profile;
    std::vector<ProcessInfo> processes;
    KernelInfo kernelInfo;

    // Page table caching (512 entries = 4KB per table)
    struct PageTable {
        uint64_t entries[512];
        bool valid = false;
    };
    std::unordered_map<uint64_t, PageTable> pageTableCache;  // Key = base PA (page-aligned)

    /**
     * Check if memory location looks like valid EPROCESS structure
     */
    bool IsValidEPROCESS(const uint8_t* data, uint64_t physAddr) {
        // 1. Check PID is reasonable
        uint32_t pid = *reinterpret_cast<const uint32_t*>(data + profile.eprocess_unique_process_id);

        if (pid == 0 || pid > 100000) {
            return false;
        }

        // 2. Check DirectoryTableBase (CR3) is a valid physical address
        uint64_t dtb_raw = *reinterpret_cast<const uint64_t*>(data + profile.kprocess_directory_table_base);
        // Mask to get physical address only (bits 12-51), CR3 has flags in bits 0-11 and 52-63
        uint64_t dtb = dtb_raw & 0x000FFFFFFFFFF000ULL;

        // DTB should be:
        // - Non-zero
        // - Within actual physical memory range (not kernel VA)
        // - Not suspiciously round values like exactly 4GB
        if (dtb == 0) {
            return false;
        }
        if (dtb >= 0xFFFF000000000000ULL) {
            return false;
        }
        // DTB must be within physical RAM range (0-2GB OR 4-10GB, NOT in PCI hole)
        // Maximum physical address is 0x280000000 (10GB)
        const uint64_t MAX_PHYSICAL_ADDR = 0x280000000ULL;  // 10GB
        if (dtb >= MAX_PHYSICAL_ADDR) {
            return false;
        }
        // Also reject DTBs in the PCI hole (2-4GB range)
        if (dtb >= 0x80000000ULL && dtb < 0x100000000ULL) {
            return false;
        }
        if (dtb == 0x100000000ULL) {
            return false;
        }
        if (dtb == 0x80000000ULL) {
            return false;
        }

        // 3. Check process name at ImageFileName offset
        const char* name = reinterpret_cast<const char*>(data + profile.eprocess_image_file_name);

        // Must be printable ASCII
        constexpr size_t IMAGE_FILE_NAME_SIZE = 15;
        for (size_t i = 0; i < IMAGE_FILE_NAME_SIZE; i++) {
            if (name[i] == '\0') break;
            if (name[i] < 0x20 || name[i] > 0x7E) {
                return false;
            }
        }

        // Check if name looks like a known process
        std::string procName(name, IMAGE_FILE_NAME_SIZE);
        procName = procName.substr(0, procName.find('\0'));

        // Name validation: must be at least 2 chars and have .exe or be System
        if (procName.length() < 2) {
            return false;
        }

        bool isExe = (procName.find(".exe") != std::string::npos);
        bool isSystem = (procName == "System");
        if (!isExe && !isSystem) {
            return false;
        }

        // 4. ActiveProcessLinks Flink should be a kernel VA
        uint64_t flink = *reinterpret_cast<const uint64_t*>(data + profile.eprocess_active_process_links);
        // Flink should be in kernel VA range (0xFFFF...)
        if (flink != 0 && flink < 0xFFFF000000000000ULL) {
            return false;
        }

        return true;
    }

    /**
     * Extract process information from EPROCESS structure
     */
    bool ExtractProcessInfo(const uint8_t* data, uint64_t physAddr, ProcessInfo& proc) {
        // Extract PID (4 bytes on x64 Windows)
        uint32_t pid = *reinterpret_cast<const uint32_t*>(data + profile.eprocess_unique_process_id);

        // PID validation
        if (pid == 0 || pid > 100000) return false;

        // Extract process name
        constexpr size_t IMAGE_FILE_NAME_SIZE = 15;
        const char* name = reinterpret_cast<const char*>(data + profile.eprocess_image_file_name);
        std::string procName(name, IMAGE_FILE_NAME_SIZE);
        procName = procName.substr(0, procName.find('\0'));

        if (procName.empty()) return false;

        // Check for duplicates
        for (const auto& existing : processes) {
            if (existing.pid == pid && existing.comm == procName) {
                return false;  // Duplicate
            }
        }

        // Fill process info
        proc.task_addr = physAddr;
        proc.pid = pid;
        proc.tgid = pid;  // Windows doesn't have separate thread group ID
        proc.comm = procName;
        proc.has_mm = true;
        proc.is_kernel_thread = (pid == 4);  // System process
        proc.mm_addr = 0;  // TODO: Extract PEB address
        proc.pgd = 0;  // Will be filled by ExtractProcessPGDs()

        return true;
    }

    /**
     * Convert physical address to file offset (accounting for PCI hole on x86-64)
     */
    int64_t PhysAddrToFileOffset(uint64_t physAddr) {
        // x86-64 memory layout with PCI hole at 2-4GB:
        // Physical 0x0-0x7FFFFFFF (0-2GB) → File 0x0-0x7FFFFFFF
        // Physical 0x80000000-0xFFFFFFFF (2-4GB) → PCI hole (NOT in file)
        // Physical 0x100000000-0x27FFFFFFF (4-10GB) → File 0x80000000-0x1FFFFFFFF

        if (physAddr < 0x80000000ULL) {
            // Below PCI hole - direct mapping
            return physAddr;
        } else if (physAddr < 0x100000000ULL) {
            // In PCI hole - not in file
            return -1;
        } else if (physAddr < 0x280000000ULL) {
            // Above PCI hole - subtract 2GB gap
            return physAddr - 0x80000000ULL;
        } else {
            // Beyond our RAM
            return -1;
        }
    }

    /**
     * Convert file offset to physical address (accounting for PCI hole on x86-64)
     */
    uint64_t FileOffsetToPhysAddr(uint64_t fileOffset) {
        // Reverse of PhysAddrToFileOffset
        if (fileOffset < 0x80000000ULL) {
            // First 2GB of file → Physical 0-2GB
            return fileOffset;
        } else {
            // Rest of file → Physical 4GB+
            return fileOffset + 0x80000000ULL;
        }
    }

    /**
     * Read physical memory (from file or via QMP)
     */
    bool ReadPhysicalMemory(uint64_t physAddr, uint8_t* buffer, size_t size) {
        // Convert physical address to file offset
        int64_t fileOffset = PhysAddrToFileOffset(physAddr);

        // Try memory file first
        if (fileOffset >= 0 && fileOffset + size <= memorySize) {
            ssize_t bytesRead = pread(memfd, buffer, size, fileOffset);
            return (bytesRead == static_cast<ssize_t>(size));
        }

        // Fall back to QMP if available
        if (qmp && qmp->IsConnected()) {
            std::cerr << "[QMP] Reading beyond 8GB: addr=0x" << std::hex << physAddr
                     << " size=" << std::dec << size << std::endl;
            std::vector<uint8_t> vec(size);
            bool success = qmp->ReadMemory(physAddr, size, vec);
            if (success) {
                memcpy(buffer, vec.data(), size);
                std::cerr << "[QMP] Success! First byte: 0x" << std::hex
                         << (int)vec[0] << std::dec << std::endl;
            } else {
                std::cerr << "[QMP] Failed to read!" << std::endl;
            }
            return success;
        }

        return false;
    }

    /**
     * Read physical memory for page tables (use QMP JSON protocol with caching)
     * Page tables are NOT in memory-backend-file on Windows!
     *
     * Optimization: Cache entire 4KB page tables (512 entries) to reduce QMP overhead
     */
    bool ReadPageTableEntry(uint64_t physAddr, uint8_t* buffer, size_t size) {
        if (size != 8) {
            std::cerr << "[ReadPageTableEntry] Only 8-byte reads supported" << std::endl;
            return false;
        }

        // Calculate page table base and index
        uint64_t tableBase = physAddr & ~0xFFFULL;  // Page-aligned base
        size_t index = (physAddr & 0xFFF) / 8;      // Entry index (0-511)

        // Check if we have this page table cached
        auto it = pageTableCache.find(tableBase);
        if (it != pageTableCache.end() && it->second.valid) {
            // Cache hit!
            memcpy(buffer, &it->second.entries[index], 8);
            return true;
        }

        // Cache miss - read entire page table (4KB = 512 entries) from memory file
        uint8_t tableData[4096];
        bool success = ReadPhysicalMemory(tableBase, tableData, 4096);

        if (success) {
            // Store entire page table in cache
            PageTable& table = pageTableCache[tableBase];
            memcpy(table.entries, tableData, 4096);
            table.valid = true;

            // Return requested entry
            memcpy(buffer, &table.entries[index], 8);
            return true;
        }

        return false;
    }

    /**
     * Scan all physical memory for EPROCESS structures
     * Returns number of processes found
     */
    size_t ScanForAllProcesses() {
        const size_t SCAN_CHUNK = 4096 * 1024;  // 4MB chunks
        std::vector<uint8_t> buffer(SCAN_CHUNK);
        size_t count = 0;

        // We need to read enough bytes to validate EPROCESS structure
        const size_t MIN_EPROCESS_SIZE = profile.eprocess_image_file_name + 16;

        std::cout << "[WindowsKernelDiscovery] Scanning " << (memorySize / (1024*1024))
                  << "MB of memory..." << std::endl;

        for (size_t offset = 0; offset < memorySize; offset += SCAN_CHUNK) {
            size_t chunkSize = std::min(SCAN_CHUNK, memorySize - offset);

            ssize_t bytesRead = pread(memfd, buffer.data(), chunkSize, offset);
            if (bytesRead <= 0) continue;

            // Progress indicator every 256MB
            if (offset % (256 * 1024 * 1024) == 0 && offset > 0) {
                std::cout << "  Scanned " << (offset / (1024*1024)) << "MB, found "
                         << count << " processes so far..." << std::endl;
            }

            // Scan for EPROCESS structures at 8-byte aligned offsets
            // This covers both 16-byte aligned (0, 16, 32...) and 8-byte aligned (8, 24, 40...)
            for (size_t i = 0; i + MIN_EPROCESS_SIZE < bytesRead; i += 8) {
                // Convert file offset to physical address (accounting for PCI hole)
                uint64_t physAddr = FileOffsetToPhysAddr(offset + i);

                // Validate using IsValidEPROCESS
                if (!IsValidEPROCESS(buffer.data() + i, physAddr)) {
                    continue;
                }

                // Extract process info
                ProcessInfo proc;
                if (ExtractProcessInfo(buffer.data() + i, physAddr, proc)) {
                    processes.push_back(proc);
                    count++;
                }
            }
        }

        return count;
    }

    /**
     * Find all System process candidates (PID 4, name "System")
     * Returns vector of physical addresses
     */
    std::vector<uint64_t> FindAllSystemProcesses() {
        const size_t SCAN_CHUNK = 4096 * 1024;  // 4MB chunks
        std::vector<uint8_t> buffer(SCAN_CHUNK);

        std::vector<uint64_t> candidates;

        for (size_t offset = 0; offset < memorySize; offset += SCAN_CHUNK) {
            size_t chunkSize = std::min(SCAN_CHUNK, memorySize - offset);

            ssize_t bytesRead = pread(memfd, buffer.data(), chunkSize, offset);
            if (bytesRead <= 0) continue;

            // Scan for PID=4 at the correct offset
            // EPROCESS structures are pool-allocated and 16-byte aligned
            for (size_t i = 0; i + profile.eprocess_size < bytesRead; i += 16) {
                // Check PID at offset
                uint32_t pid = *reinterpret_cast<const uint32_t*>(buffer.data() + i + profile.eprocess_unique_process_id);
                if (pid != 4) continue;

                // Check name at offset
                const char* name = reinterpret_cast<const char*>(buffer.data() + i + profile.eprocess_image_file_name);
                if (strncmp(name, "System", 6) == 0 && name[6] == '\0') {
                    // Convert file offset to physical address (accounting for PCI hole)
                    uint64_t addr = FileOffsetToPhysAddr(offset + i);

                    // Read and validate DirectoryTableBase BEFORE adding as candidate
                    uint64_t dtb_offset = profile.kprocess_directory_table_base;
                    if (i + dtb_offset + 8 < bytesRead) {
                        uint64_t dtb_raw = *reinterpret_cast<const uint64_t*>(buffer.data() + i + dtb_offset);

                        // Mask to get physical address only (bits 12-51)
                        // CR3 has flags in bits 0-11 and 52-63
                        uint64_t dtb = dtb_raw & 0x000FFFFFFFFFF000ULL;

                        // Validate DTB: non-zero and within physical RAM (0-2GB OR 4-10GB, NOT PCI hole)
                        const uint64_t MAX_PHYSICAL_ADDR = 0x280000000ULL;  // 10GB
                        bool dtbInValidRange = (dtb != 0 && dtb < MAX_PHYSICAL_ADDR &&
                                               !(dtb >= 0x80000000ULL && dtb < 0x100000000ULL));
                        if (dtbInValidRange) {
                            // Further validate: Check PML4[256] for kernel mappings
                            // System process must have kernel text mapped in upper half
                            uint8_t pml4_256_data[8];
                            uint64_t pml4_256_addr = dtb + (256 * 8);

                            if (ReadPhysicalMemory(pml4_256_addr, pml4_256_data, 8)) {
                                uint64_t pml4e256 = *reinterpret_cast<uint64_t*>(pml4_256_data);
                                uint64_t pdpt_addr = pml4e256 & 0x000FFFFFFFFFF000ULL;

                                // PML4[256] should be present and point to valid PDPT within physical RAM
                                bool pdptInValidRange = (pdpt_addr > 0 && pdpt_addr < MAX_PHYSICAL_ADDR &&
                                                        !(pdpt_addr >= 0x80000000ULL && pdpt_addr < 0x100000000ULL));
                                if ((pml4e256 & 0x1) && pdptInValidRange) {
                                    candidates.push_back(addr);
                                    std::cout << "[WindowsKernelDiscovery] Found System candidate at PA 0x"
                                              << std::hex << addr << " DTB=0x" << dtb
                                              << " PML4[256]=0x" << pml4e256 << std::dec << std::endl;
                                }
                            }
                        }
                    }
                }
            }
        }

        return candidates;
    }

    /**
     * Walk the ActiveProcessLinks doubly-linked list
     * Returns number of processes found
     */
    size_t WalkProcessList(uint64_t systemEprocessAddr) {
        std::set<uint64_t> visited;
        uint64_t current = systemEprocessAddr;
        size_t count = 0;
        const size_t MAX_PROCESSES = 1000;  // Safety limit

        // Calculate safe read size - only what we need
        const size_t EPROCESS_READ_SIZE = profile.eprocess_active_process_links + 16;  // Just enough for links

        std::cout << "[WindowsKernelDiscovery] EPROCESS read size: " << EPROCESS_READ_SIZE << " bytes" << std::endl;

        while (count < MAX_PROCESSES) {
            std::cout << "[WindowsKernelDiscovery] Reading EPROCESS at PA 0x" << std::hex << current << std::dec << std::endl;

            // Check if we've seen this address before (circular list detection)
            if (visited.count(current) > 0) {
                std::cout << "[WindowsKernelDiscovery] Circular list detected, stopping" << std::endl;
                break;
            }
            visited.insert(current);

            // Read enough of EPROCESS to get PID, name, and links
            std::vector<uint8_t> eprocData(EPROCESS_READ_SIZE);
            if (!ReadPhysicalMemory(current, eprocData.data(), EPROCESS_READ_SIZE)) {
                std::cerr << "[WindowsKernelDiscovery] Failed to read EPROCESS at 0x"
                          << std::hex << current << std::dec << std::endl;
                break;
            }

            // Extract process info
            ProcessInfo proc;
            if (ExtractProcessInfo(eprocData.data(), current, proc)) {
                processes.push_back(proc);
                count++;
            }

            // Get Flink (forward link) from ActiveProcessLinks
            // ActiveProcessLinks is a LIST_ENTRY with Flink at offset 0
            uint64_t activeProcessLinksAddr = current + profile.eprocess_active_process_links;
            uint64_t flink = *reinterpret_cast<const uint64_t*>(eprocData.data() + profile.eprocess_active_process_links);

            // Validate Flink looks reasonable (should be in our memory range or kernel space)
            if (flink == 0 || flink < profile.eprocess_active_process_links) {
                std::cerr << "[WindowsKernelDiscovery] Invalid Flink: 0x" << std::hex << flink << std::dec << std::endl;
                break;
            }

            // Flink points to the next ActiveProcessLinks, so subtract offset to get EPROCESS base
            uint64_t nextEprocess = flink - profile.eprocess_active_process_links;

            // Validate next EPROCESS address
            if (nextEprocess >= memorySize && nextEprocess < 0xFFFF000000000000ULL) {
                std::cerr << "[WindowsKernelDiscovery] Next EPROCESS out of range: 0x"
                          << std::hex << nextEprocess << std::dec << std::endl;
                break;
            }

            // Stop if we've looped back to the start
            if (nextEprocess == systemEprocessAddr) {
                break;
            }

            current = nextEprocess;
        }

        return count;
    }

    /**
     * Walk the ActiveProcessLinks list using VA→PA translation
     * Returns number of processes found
     */
    size_t WalkProcessListWithTranslation(uint64_t systemEprocessPA, uint64_t kernelCR3) {
        std::set<uint64_t> visitedVAs;
        size_t count = 0;
        const size_t MAX_PROCESSES = 1000;
        const size_t EPROCESS_SIZE = 2816;  // Full EPROCESS size

        // Get first process VA (System process itself)
        // We need to read ActiveProcessLinks from System to get the Flink VA
        uint8_t linksData[16];
        if (!ReadPhysicalMemory(systemEprocessPA + profile.eprocess_active_process_links, linksData, 16)) {
            std::cerr << "[WindowsKernelDiscovery] Failed to read System ActiveProcessLinks" << std::endl;
            return 0;
        }

        uint64_t firstFlink = *reinterpret_cast<uint64_t*>(linksData);

        std::cout << "[WindowsKernelDiscovery] Read Flink from System: 0x" << std::hex << firstFlink << std::dec << std::endl;
        std::cout << "[WindowsKernelDiscovery] ActiveProcessLinks offset: " << profile.eprocess_active_process_links << std::endl;

        uint64_t systemEprocessVA = firstFlink - profile.eprocess_active_process_links;  // Calculate System VA

        std::cout << "[WindowsKernelDiscovery] System EPROCESS VA: 0x" << std::hex << systemEprocessVA << std::dec << std::endl;

        // Validate the VA looks like a kernel address
        if ((systemEprocessVA & 0xFFFF000000000000ULL) == 0) {
            std::cerr << "[WindowsKernelDiscovery] WARNING: System EPROCESS VA doesn't look like kernel address!" << std::endl;
            std::cerr << "[WindowsKernelDiscovery] Expected VA to start with 0xFFFF..., got 0x" << std::hex << systemEprocessVA << std::dec << std::endl;
            // This means our System EPROCESS PA is probably misaligned
            return 0;
        }

        uint64_t currentVA = systemEprocessVA;

        while (count < MAX_PROCESSES) {
            // Check for circular list
            if (visitedVAs.count(currentVA) > 0) {
                std::cout << "[WindowsKernelDiscovery] Circular list detected at VA 0x" << std::hex << currentVA << std::dec << std::endl;
                break;
            }
            visitedVAs.insert(currentVA);

            // Translate VA to PA using manual page table walking with the user DTB
            // (User DTBs have kernel mappings in PML4 entries 256-511)
            uint64_t currentPA = TranslateVA(currentVA, kernelCR3);
            if (currentPA == 0) {
                std::cerr << "[WindowsKernelDiscovery] Failed to translate VA 0x" << std::hex << currentVA << std::dec << std::endl;
                break;
            }

            std::cout << "[WindowsKernelDiscovery] Translated VA 0x" << std::hex << currentVA
                      << " → PA 0x" << currentPA << std::dec << std::endl;

            // Read EPROCESS
            std::vector<uint8_t> eprocData(EPROCESS_SIZE);
            if (!ReadPhysicalMemory(currentPA, eprocData.data(), EPROCESS_SIZE)) {
                std::cerr << "[WindowsKernelDiscovery] Failed to read EPROCESS at PA 0x" << std::hex << currentPA << std::dec << std::endl;
                break;
            }

            // Extract process info
            ProcessInfo proc;
            if (ExtractProcessInfo(eprocData.data(), currentPA, proc)) {
                processes.push_back(proc);
                count++;
            }

            // Get Flink to next process
            uint64_t flink = *reinterpret_cast<const uint64_t*>(eprocData.data() + profile.eprocess_active_process_links);
            if (flink == 0) {
                std::cerr << "[WindowsKernelDiscovery] Null Flink encountered" << std::endl;
                break;
            }

            // Calculate next EPROCESS VA
            uint64_t nextVA = flink - profile.eprocess_active_process_links;

            // Stop if we've looped back to the start
            if (nextVA == systemEprocessVA) {
                std::cout << "[WindowsKernelDiscovery] Completed full circle back to System process" << std::endl;
                break;
            }

            currentVA = nextVA;
        }

        return count;
    }

    /**
     * Extract modules from PEB (Process Environment Block)
     *
     * Walks PEB→Ldr→InLoadOrderModuleList to extract all loaded modules.
     * First entry is always the main .exe, followed by DLLs.
     *
     * @param proc Process information (contains PID, EPROCESS address, DTB)
     * @param sections Output vector to append module entries
     */
    void ExtractModulesFromPEB(const ProcessInfo& proc, std::vector<MemorySection>& sections) {
        // Read PEB pointer from EPROCESS.Peb
        fprintf(stderr, "[ExtractPEB] PID %d: Starting PEB extraction (EPROCESS.Peb offset=%zu)\n", proc.pid, profile.eprocess_peb);

        uint64_t pebVA = 0;
        if (!ReadPhysicalMemory(proc.task_addr + profile.eprocess_peb, (uint8_t*)&pebVA, 8)) {
            fprintf(stderr, "[ExtractPEB] PID %d: Failed to read PEB pointer\n", proc.pid);
            return;  // Failed to read PEB pointer
        }

        fprintf(stderr, "[ExtractPEB] PID %d: PEB VA = 0x%lx\n", proc.pid, pebVA);

        // On Windows x86-64, user-mode addresses can have high bits set due to ASLR
        // Kernel addresses have top 16 bits = 0xFFFF, user-mode should not
        if (pebVA == 0 || (pebVA & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL) {
            fprintf(stderr, "[ExtractPEB] PID %d: Invalid PEB pointer (null or kernel VA)\n", proc.pid);
            return;  // Invalid PEB pointer
        }

        // Translate PEB VA to PA using process DTB
        uint64_t dtb = 0;
        if (!ReadPhysicalMemory(proc.task_addr + profile.kprocess_directory_table_base, (uint8_t*)&dtb, 8)) {
            return;  // Failed to read DTB
        }

        uint64_t pebPA = TranslateVA(pebVA, dtb);
        if (pebPA == 0) {
            return;  // PEB not mapped
        }

        // Read PEB.Ldr pointer (offset 24 in PEB)
        uint64_t ldrVA = 0;
        if (!ReadPhysicalMemory(pebPA + 24, (uint8_t*)&ldrVA, 8)) {
            return;  // Failed to read Ldr pointer
        }

        if (ldrVA == 0) {
            return;  // PEB not fully initialized
        }

        uint64_t ldrPA = TranslateVA(ldrVA, dtb);
        if (ldrPA == 0) {
            return;  // Ldr not mapped
        }

        // Read PEB_LDR_DATA.InLoadOrderModuleList head (offset 16)
        uint64_t listHead[2];  // Flink, Blink
        if (!ReadPhysicalMemory(ldrPA + 16, (uint8_t*)listHead, 16)) {
            return;  // Failed to read module list head
        }

        uint64_t currentVA = listHead[0];  // Flink points to first module
        uint64_t headVA = ldrVA + 16;  // Address of list head itself

        int moduleCount = 0;
        const int MAX_MODULES = 200;  // Safety limit

        while (currentVA != headVA && moduleCount < MAX_MODULES) {
            // Translate LDR_DATA_TABLE_ENTRY VA to PA
            uint64_t entryPA = TranslateVA(currentVA, dtb);
            if (entryPA == 0) {
                break;  // Entry not mapped
            }

            // Read LDR_DATA_TABLE_ENTRY (we need first 120 bytes)
            uint8_t entryBuffer[120];
            if (!ReadPhysicalMemory(entryPA, entryBuffer, 120)) {
                break;  // Failed to read entry
            }

            // Extract Flink for next iteration
            uint64_t flink = *reinterpret_cast<uint64_t*>(entryBuffer + 0);

            // Extract DllBase (offset 48)
            uint64_t dllBase = *reinterpret_cast<uint64_t*>(entryBuffer + 48);

            // Extract SizeOfImage (offset 64)
            uint32_t sizeOfImage = *reinterpret_cast<uint32_t*>(entryBuffer + 64);

            // Extract BaseDllName UNICODE_STRING (offset 88)
            // UNICODE_STRING: Length (2 bytes), MaxLength (2 bytes), reserved (4 bytes), Buffer (8 bytes)
            uint16_t nameLength = *reinterpret_cast<uint16_t*>(entryBuffer + 88);
            uint64_t nameBufferVA = *reinterpret_cast<uint64_t*>(entryBuffer + 96);

            if (dllBase != 0 && sizeOfImage > 0 && nameLength > 0 && nameLength < 512) {
                // Translate name buffer VA to PA
                uint64_t nameBufferPA = TranslateVA(nameBufferVA, dtb);
                if (nameBufferPA != 0) {
                    // Read Unicode name (UTF-16LE)
                    std::vector<uint16_t> nameUTF16(nameLength / 2 + 1, 0);
                    if (ReadPhysicalMemory(nameBufferPA, (uint8_t*)nameUTF16.data(), nameLength)) {
                        // Convert UTF-16LE to ASCII (simple conversion, assumes ASCII subset)
                        std::string moduleName;
                        moduleName.reserve(nameLength / 2);
                        for (size_t i = 0; i < nameLength / 2; i++) {
                            char c = (char)nameUTF16[i];
                            if (c >= 32 && c < 127) {
                                moduleName += c;
                            } else {
                                moduleName += '?';  // Non-ASCII character
                            }
                        }

                        // Add module entry to sections
                        MemorySection entry;
                        entry.start = dllBase;
                        entry.end = dllBase + sizeOfImage;
                        entry.flags = 0x07;  // RWX (we don't have detailed permissions from PEB)
                        entry.name = moduleName;
                        entry.type = MemorySection::SHARED_LIB;  // Assume DLL/shared lib

                        sections.push_back(entry);
                        moduleCount++;

                        // Show first 5 modules for verification
                        if (moduleCount <= 5) {
                            fprintf(stderr, "[ExtractPEB] PID %d: Module #%d: %s (base=0x%lx, size=0x%x)\n",
                                    proc.pid, moduleCount, moduleName.c_str(), dllBase, sizeOfImage);
                        }
                    }
                }
            }

            // Move to next entry
            currentVA = flink;

            // Safety check: detect circular list
            if (currentVA == listHead[0] && moduleCount > 0) {
                break;  // We've looped back to the start
            }
        }

        fprintf(stderr, "[ExtractPEB] PID %d: Found %d modules from PEB\n", proc.pid, moduleCount);
    }

    /**
     * Extract filename from VAD structure by following pointer chain
     *
     * Chain: MMVAD → Subsection → ControlArea → FILE_OBJECT → FileName
     *
     * @param vad_pa Physical address of MMVAD structure
     * @return Filename string, or empty if not file-backed
     */
    std::string ExtractVADFilename(uint64_t vad_pa) {
        // Read full MMVAD structure (128 bytes) to get Subsection pointer
        constexpr size_t MMVAD_SIZE = 128;
        uint8_t mmvad_buffer[MMVAD_SIZE];

        if (!ReadPhysicalMemory(vad_pa, mmvad_buffer, MMVAD_SIZE)) {
            fprintf(stderr, "[ExtractVAD] FAILED: Cannot read MMVAD at PA 0x%lx\n", vad_pa);
            return "";  // Failed to read MMVAD
        }

        // Debug: Dump first 128 bytes of MMVAD to find kernel pointers
        fprintf(stderr, "[ExtractVAD] MMVAD at PA 0x%lx dump:\n", vad_pa);
        for (size_t i = 0; i < MMVAD_SIZE; i += 16) {
            fprintf(stderr, "  +0x%02lx: ", i);
            for (size_t j = 0; j < 16 && (i + j) < MMVAD_SIZE; j++) {
                fprintf(stderr, "%02x ", mmvad_buffer[i + j]);
            }
            fprintf(stderr, "\n");
        }

        // Subsection pointer is at offset 72 in MMVAD
        uint64_t subsection_va = *reinterpret_cast<uint64_t*>(mmvad_buffer + 72);

        fprintf(stderr, "[ExtractVAD] MMVAD at PA 0x%lx -> Subsection VA 0x%lx (from offset 72)\n", vad_pa, subsection_va);

        // Validate it's a kernel VA
        if ((subsection_va >> 48) != 0xffff || subsection_va == 0) {
            fprintf(stderr, "[ExtractVAD] FAILED: Subsection VA invalid (not kernel pointer or NULL)\n");
            return "";  // Not a file-backed VAD
        }

        // Translate Subsection VA to PA using System DTB
        uint64_t subsection_pa = TranslateVA(subsection_va, kernelInfo.swapper_pgd);
        if (subsection_pa == 0) {
            return "";  // Translation failed
        }

        // Read SUBSECTION structure (first 64 bytes)
        uint8_t subsection_buffer[64];
        if (!ReadPhysicalMemory(subsection_pa, subsection_buffer, 64)) {
            return "";  // Failed to read SUBSECTION
        }

        // ControlArea pointer is at offset 0 in SUBSECTION
        uint64_t control_area_va = *reinterpret_cast<uint64_t*>(subsection_buffer + 0);
        if ((control_area_va >> 48) != 0xffff || control_area_va == 0) {
            return "";  // Invalid ControlArea VA
        }

        // Translate ControlArea VA to PA
        uint64_t control_area_pa = TranslateVA(control_area_va, kernelInfo.swapper_pgd);
        if (control_area_pa == 0) {
            return "";  // Translation failed
        }

        // Read CONTROL_AREA structure (first 128 bytes)
        uint8_t control_area_buffer[128];
        if (!ReadPhysicalMemory(control_area_pa, control_area_buffer, 128)) {
            return "";  // Failed to read CONTROL_AREA
        }

        // FilePointer (_EX_FAST_REF) is at offset 0x40 (64) in CONTROL_AREA (Windows 11 25H2/26200)
        // _EX_FAST_REF encodes pointer in bits 4-63, refcount in bits 0-3
        uint64_t file_object_va_raw = *reinterpret_cast<uint64_t*>(control_area_buffer + 0x40);
        uint64_t file_object_va = file_object_va_raw & ~0xFULL;  // Mask lower 4 bits (refcount)

        if ((file_object_va >> 48) != 0xffff || file_object_va == 0) {
            return "";  // Not a file-backed section (NULL or invalid pointer)
        }

        fprintf(stderr, "[ExtractVAD] ControlArea PA 0x%lx -> FILE_OBJECT VA 0x%lx (raw: 0x%lx)\n",
                control_area_pa, file_object_va, file_object_va_raw);

        // Translate FILE_OBJECT VA to PA
        uint64_t file_object_pa = TranslateVA(file_object_va, kernelInfo.swapper_pgd);
        if (file_object_pa == 0) {
            fprintf(stderr, "[ExtractVAD] FAILED: FILE_OBJECT VA->PA translation failed for VA 0x%lx\n", file_object_va);
            return "";  // Translation failed
        }

        fprintf(stderr, "[ExtractVAD] FILE_OBJECT translated to PA 0x%lx\n", file_object_pa);

        // Read FILE_OBJECT structure (first 128 bytes)
        uint8_t file_object_buffer[128];
        if (!ReadPhysicalMemory(file_object_pa, file_object_buffer, 128)) {
            return "";  // Failed to read FILE_OBJECT
        }

        // FileName is a UNICODE_STRING at offset 88 in FILE_OBJECT
        // UNICODE_STRING: Length (2), MaximumLength (2), padding (4), Buffer pointer (8)
        uint16_t length = *reinterpret_cast<uint16_t*>(file_object_buffer + 88);
        uint64_t buffer_va = *reinterpret_cast<uint64_t*>(file_object_buffer + 96);

        if (length == 0 || length > 512 || (buffer_va >> 48) != 0xffff) {
            return "";  // Invalid filename buffer
        }

        fprintf(stderr, "[ExtractVAD] Filename buffer VA 0x%lx, length %d\n", buffer_va, length);

        // Translate filename buffer VA to PA
        uint64_t buffer_pa = TranslateVA(buffer_va, kernelInfo.swapper_pgd);
        if (buffer_pa == 0) {
            fprintf(stderr, "[ExtractVAD] FAILED: Filename buffer VA->PA translation failed for VA 0x%lx\n", buffer_va);
            return "";  // Translation failed
        }

        fprintf(stderr, "[ExtractVAD] Filename buffer translated to PA 0x%lx\n", buffer_pa);

        // Read Unicode filename
        uint8_t filename_buffer[512];
        size_t bytes_to_read = std::min(static_cast<size_t>(length), sizeof(filename_buffer));
        if (!ReadPhysicalMemory(buffer_pa, filename_buffer, bytes_to_read)) return "";

        // Convert Unicode (UTF-16LE) to ASCII
        std::string filename;
        for (size_t i = 0; i + 1 < bytes_to_read; i += 2) {
            wchar_t wc = filename_buffer[i] | (filename_buffer[i+1] << 8);
            if (wc < 128 && wc != 0) {
                filename += static_cast<char>(wc);
            } else if (wc == 0) {
                break;
            }
        }

        // Extract just the filename from full path
        size_t last_slash = filename.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            filename = filename.substr(last_slash + 1);
        }

        fprintf(stderr, "[ExtractVAD] SUCCESS: Extracted filename '%s'\n", filename.c_str());
        return filename;
    }

    /**
     * Extract PTEs for all memory sections in a process
     *
     * @param proc Process to extract PTEs for
     */
    void ExtractPTEsForProcess(ProcessInfo& proc) {
        if (proc.pgd == 0) {
            std::cerr << "[ExtractPTEs] Process PGD is 0, skipping PTE extraction" << std::endl;
            return;
        }

        const uint64_t PAGE_SIZE = 4096;  // 4KB pages
        size_t total_pages = 0;
        size_t extracted_ptes = 0;

        // Iterate through each memory section
        for (const auto& section : proc.sections) {
            uint64_t start_va = section.start;
            uint64_t end_va = section.end;

            // Calculate number of pages in this section
            size_t section_pages = (end_va - start_va + PAGE_SIZE - 1) / PAGE_SIZE;
            total_pages += section_pages;

            // Limit per-section extraction to avoid excessive output
            // Increased from 1024 to handle large DLLs (some are 10-20MB)
            size_t max_pages_per_section = 8192;  // Extract up to 8192 pages (32MB) per section
            size_t pages_to_extract = std::min(section_pages, max_pages_per_section);

            // Walk page tables for each page in the section
            for (uint64_t va = start_va; va < end_va && (va - start_va) / PAGE_SIZE < pages_to_extract; va += PAGE_SIZE) {
                // Translate VA to PA using process DTB
                uint64_t pa = TranslateVA(va, proc.pgd);

                if (pa != 0) {
                    // Create PTE entry
                    PTE pte;
                    pte.va = va;
                    pte.pa = pa;
                    pte.size = PAGE_SIZE;
                    pte.present = true;
                    pte.writable = true;   // TODO: Parse page table flags
                    pte.executable = false; // TODO: Parse NX bit

                    proc.ptes.push_back(pte);
                    extracted_ptes++;
                }
            }
        }

        std::cout << "[ExtractPTEs] Process pages: " << total_pages
                  << ", extracted: " << extracted_ptes << std::endl;
    }

    /**
     * Recursively walk VAD tree (AVL tree of memory regions)
     *
     * @param vad_pa Physical address of MMVAD node
     * @param sections Output vector of memory sections
     */
    void WalkVADTree(uint64_t vad_pa, std::vector<MemorySection>& sections) {
        // Read MMVAD_SHORT structure (64 bytes minimum)
        // Layout: RTL_BALANCED_NODE (24 bytes) + StartingVpn (4) + EndingVpn (4) + ... + VadFlags (at 48)
        //
        // VAD nodes ARE accessible via memory-backend-file with correct PA→file offset mapping
        // Use ReadPhysicalMemory which has PCI hole-aware offset calculation
        constexpr size_t MMVAD_READ_SIZE = 64;
        uint8_t vad_data[MMVAD_READ_SIZE];

        if (!ReadPhysicalMemory(vad_pa, vad_data, MMVAD_READ_SIZE)) {
            std::cerr << "[WalkVADTree] Failed to read VAD at PA 0x" << std::hex << vad_pa << std::dec << std::endl;
            return;
        }

        // Extract Left and Right child pointers (offsets 0 and 8 within RTL_BALANCED_NODE)
        uint64_t left_va = *reinterpret_cast<uint64_t*>(vad_data + 0);
        uint64_t right_va = *reinterpret_cast<uint64_t*>(vad_data + 8);

        std::cerr << "[WalkVADTree] VAD at PA 0x" << std::hex << vad_pa << " Left=0x" << left_va << " Right=0x" << right_va << std::dec << std::endl;

        // Extract StartingVpn and EndingVpn (offsets 24 and 28)
        uint32_t starting_vpn = *reinterpret_cast<uint32_t*>(vad_data + 24);
        uint32_t ending_vpn = *reinterpret_cast<uint32_t*>(vad_data + 28);

        // Extract VadFlags (offset 48) for protection bits
        uint32_t vad_flags = *reinterpret_cast<uint32_t*>(vad_data + 48);

        // Convert VPN to virtual address (VPN = VA >> 12)
        uint64_t start_va = static_cast<uint64_t>(starting_vpn) << 12;
        uint64_t end_va = ((static_cast<uint64_t>(ending_vpn) + 1) << 12) - 1;

        // Add this VAD's region to sections
        MemorySection section;
        section.start = start_va;
        section.end = end_va;
        section.flags = vad_flags;

        // Parse VadFlags to determine type
        // VadFlags bits: 0-1=MemCommit, 2-4=VadType, 5-9=Protection
        uint32_t vad_type = (vad_flags >> 2) & 0x7;  // Extract bits 2-4
        fprintf(stderr, "[WalkVAD] VAD at PA 0x%lx has VadType=%u (flags=0x%x)\n", vad_pa, vad_type, vad_flags);

        switch (vad_type) {
            case 0:  // Private memory (heap, stack, etc.)
                section.type = MemorySection::ANONYMOUS;
                section.name = "[private]";
                break;
            case 1:  // Mapped file
                fprintf(stderr, "[WalkVAD] VAD type 1 (Mapped file) at PA 0x%lx - calling ExtractVADFilename\n", vad_pa);
                section.type = MemorySection::FILE_BACKED;
                section.name = ExtractVADFilename(vad_pa);
                if (section.name.empty()) {
                    section.name = "[mapped]";
                }
                break;
            case 2:  // Image/Section (DLL, EXE)
                fprintf(stderr, "[WalkVAD] VAD type 2 (Image/DLL) at PA 0x%lx - calling ExtractVADFilename\n", vad_pa);
                section.type = MemorySection::SHARED_LIB;
                section.name = ExtractVADFilename(vad_pa);
                if (section.name.empty()) {
                    section.name = "[image]";
                }
                break;
            default:
                section.type = MemorySection::UNKNOWN;
                section.name = "[unknown]";
                break;
        }

        sections.push_back(section);

        // Recursively walk left child
        if (left_va != 0 && (left_va >> 48) == 0xffff) {
            // CRITICAL: Use System DTB for kernel VA translation
            uint64_t left_pa = TranslateVA(left_va, kernelInfo.swapper_pgd);
            std::cerr << "[WalkVADTree] Left child VA=0x" << std::hex << left_va << " → PA=0x" << left_pa << std::dec << std::endl;
            if (left_pa != 0) {
                WalkVADTree(left_pa, sections);
            }
        } else if (left_va != 0) {
            std::cerr << "[WalkVADTree] Left VA=0x" << std::hex << left_va << " not a kernel VA (bits 63-48 = 0x" << (left_va >> 48) << ")" << std::dec << std::endl;
        }

        // Recursively walk right child
        if (right_va != 0 && (right_va >> 48) == 0xffff) {
            // CRITICAL: Use System DTB for kernel VA translation
            uint64_t right_pa = TranslateVA(right_va, kernelInfo.swapper_pgd);
            std::cerr << "[WalkVADTree] Right child VA=0x" << std::hex << right_va << " → PA=0x" << right_pa << std::dec << std::endl;
            if (right_pa != 0) {
                WalkVADTree(right_pa, sections);
            }
        } else if (right_va != 0) {
            std::cerr << "[WalkVADTree] Right VA=0x" << std::hex << right_va << " not a kernel VA (bits 63-48 = 0x" << (right_va >> 48) << ")" << std::dec << std::endl;
        }
    }
};

} // namespace Haywire
