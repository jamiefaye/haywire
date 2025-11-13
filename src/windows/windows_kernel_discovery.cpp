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
                if (dtb != 0 && dtb < memorySize) {
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

    // Per-process operations (stub implementations for now)
    void WalkProcessPageTables(ProcessInfo& proc) override {
        // TODO: Implement Windows page table walking
        std::cout << "[WindowsKernelDiscovery] Page table walking not yet implemented" << std::endl;
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

        std::cout << "[WindowsKernelDiscovery] VadRoot PA: 0x" << std::hex
                  << vadroot_pa << std::dec << std::endl;

        // Walk VAD tree recursively
        WalkVADTree(vadroot_pa, proc.sections);

        std::cout << "[WindowsKernelDiscovery] Found " << proc.sections.size()
                  << " memory regions for PID " << pid << std::endl;

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

        std::cout << "[TranslateVA] VA=0x" << std::hex << va
                  << " PGD=0x" << pgdBase
                  << " PML4_idx=" << pml4_index << std::dec << std::endl;
        uint64_t pt_index = (va >> 12) & 0x1FF;
        uint64_t offset = va & 0xFFF;

        // Read PML4 entry (CRITICAL: Use QMP, not file!)
        uint64_t pml4_addr = (pgdBase & 0x000FFFFFFFFFF000ULL) + (pml4_index * 8);
        uint64_t pml4e;
        if (!ReadPageTableEntry(pml4_addr, reinterpret_cast<uint8_t*>(&pml4e), 8)) {
            std::cerr << " Failed to read PML4 entry via QMP" << std::endl;
            return 0;
        }
        std::cout << " PML4E=0x" << std::hex << pml4e << std::dec
                  << " Present=" << (pml4e & 0x1) << std::endl;
        if (!(pml4e & 0x1)) return 0;  // Not present

        // Read PDPT entry - mask to get physical address (bits 12-51)
        uint64_t pdpt_addr = (pml4e & 0x000FFFFFFFFFF000ULL) + (pdpt_index * 8);
        uint64_t pdpte;
        if (!ReadPageTableEntry(pdpt_addr, reinterpret_cast<uint8_t*>(&pdpte), 8)) {
            std::cerr << " Failed to read PDPT via QMP" << std::endl;
            return 0;
        }
        std::cout << " PDPTE=0x" << std::hex << pdpte << std::dec << std::endl;

        // Validate PDPTE is not garbage (all 1's is invalid)
        if (pdpte == 0xffffffffffffffff) {
            std::cerr << " [TranslateVA] PDPTE is invalid (all 1's)" << std::endl;
            return 0;
        }

        std::cout << " Present=" << (pdpte & 0x1)
                  << " Huge=" << ((pdpte & 0x80) ? 1 : 0) << std::endl;
        if (!(pdpte & 0x1)) return 0;  // Not present
        if (pdpte & 0x80) {  // 1GB huge page
            uint64_t pa = (pdpte & 0x000FFFFFC0000000ULL) + (va & 0x3FFFFFFF);
            std::cout << " [TranslateVA] 1GB huge page → PA 0x" << std::hex << pa << std::dec << std::endl;
            return pa;
        }

        // Read PD entry - mask to get physical address (bits 12-51)
        uint64_t pd_addr = (pdpte & 0x000FFFFFFFFFF000ULL) + (pd_index * 8);
        uint64_t pde;
        if (!ReadPageTableEntry(pd_addr, reinterpret_cast<uint8_t*>(&pde), 8)) {
            std::cerr << " Failed to read PD via QMP" << std::endl;
            return 0;
        }
        std::cout << " PDE=0x" << std::hex << pde << " Present=" << (pde & 0x1)
                  << " Huge=" << ((pde & 0x80) ? 1 : 0) << std::dec << std::endl;
        if (!(pde & 0x1)) return 0;  // Not present
        if (pde & 0x80) {  // 2MB huge page
            uint64_t pa = (pde & 0x000FFFFFFFE00000ULL) + (va & 0x1FFFFF);
            std::cout << " [TranslateVA] 2MB huge page → PA 0x" << std::hex << pa << std::dec << std::endl;
            return pa;
        }

        // Read PT entry - mask to get physical address (bits 12-51)
        uint64_t pt_addr = (pde & 0x000FFFFFFFFFF000ULL) + (pt_index * 8);
        uint64_t pte;
        if (!ReadPageTableEntry(pt_addr, reinterpret_cast<uint8_t*>(&pte), 8)) {
            std::cerr << " Failed to read PT via QMP" << std::endl;
            return 0;
        }
        std::cout << " PTE=0x" << std::hex << pte << " Present=" << (pte & 0x1) << std::dec << std::endl;
        if (!(pte & 0x1)) return 0;  // Not present

        // 4KB page - mask to get physical address (bits 12-51)
        uint64_t pa = (pte & 0x000FFFFFFFFFF000ULL) + offset;
        std::cout << " [TranslateVA] 4KB page → PA 0x" << std::hex << pa << std::dec << std::endl;
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
        if (pid == 0 || pid > 100000) return false;

        // 2. Check DirectoryTableBase (CR3) is a valid physical address
        uint64_t dtb_raw = *reinterpret_cast<const uint64_t*>(data + profile.kprocess_directory_table_base);
        // Mask to get physical address only (bits 12-51), CR3 has flags in bits 0-11 and 52-63
        uint64_t dtb = dtb_raw & 0x000FFFFFFFFFF000ULL;

        // DTB should be:
        // - Non-zero
        // - Within actual physical memory range (not kernel VA)
        // - Not suspiciously round values like exactly 4GB
        if (dtb == 0) return false;
        if (dtb >= 0xFFFF000000000000ULL) return false;  // Not a kernel VA (should be impossible after mask)
        if (dtb >= memorySize) return false;  // Must be within actual RAM
        if (dtb == 0x100000000ULL) return false;  // Reject exactly 4GB (common false positive)
        if (dtb == 0x80000000ULL) return false;  // Reject exactly 2GB (common false positive)

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
        if (procName.length() < 2) return false;

        bool isExe = (procName.find(".exe") != std::string::npos);
        bool isSystem = (procName == "System");
        if (!isExe && !isSystem) return false;

        // 4. ActiveProcessLinks Flink should be a kernel VA
        uint64_t flink = *reinterpret_cast<const uint64_t*>(data + profile.eprocess_active_process_links);
        // Flink should be in kernel VA range (0xFFFF...)
        if (flink != 0 && flink < 0xFFFF000000000000ULL) return false;

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
     * Read physical memory (from file or via QMP)
     */
    bool ReadPhysicalMemory(uint64_t physAddr, uint8_t* buffer, size_t size) {
        // Try memory file first
        if (physAddr < memorySize) {
            ssize_t bytesRead = pread(memfd, buffer, size, physAddr);
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
        if (!qmp || !qmp->IsQMPConnected()) {
            std::cerr << "[ReadPageTableEntry] QMP not available!" << std::endl;
            return false;
        }

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

        // Cache miss - read entire page table (4KB = 512 entries) in one QMP call
        std::vector<uint8_t> tableData(4096);
        bool success = qmp->ReadMemoryViaQMP(tableBase, 4096, tableData);

        if (success) {
            // Store entire page table in cache
            PageTable& table = pageTableCache[tableBase];
            memcpy(table.entries, tableData.data(), 4096);
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
                uint64_t physAddr = offset + i;

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
                    uint64_t addr = offset + i;

                    // Read and validate DirectoryTableBase BEFORE adding as candidate
                    uint64_t dtb_offset = profile.kprocess_directory_table_base;
                    if (i + dtb_offset + 8 < bytesRead) {
                        uint64_t dtb_raw = *reinterpret_cast<const uint64_t*>(buffer.data() + i + dtb_offset);

                        // Mask to get physical address only (bits 12-51)
                        // CR3 has flags in bits 0-11 and 52-63
                        uint64_t dtb = dtb_raw & 0x000FFFFFFFFFF000ULL;

                        // Validate DTB: non-zero and within RAM size
                        if (dtb != 0 && dtb < memorySize) {
                            // Further validate: Check PML4[256] for kernel mappings
                            // System process must have kernel text mapped in upper half
                            uint8_t pml4_256_data[8];
                            uint64_t pml4_256_addr = dtb + (256 * 8);

                            if (ReadPhysicalMemory(pml4_256_addr, pml4_256_data, 8)) {
                                uint64_t pml4e256 = *reinterpret_cast<uint64_t*>(pml4_256_data);
                                uint64_t pdpt_addr = pml4e256 & 0x000FFFFFFFFFF000ULL;

                                // PML4[256] should be present and point to valid PDPT within RAM
                                if ((pml4e256 & 0x1) && pdpt_addr > 0 && pdpt_addr < memorySize) {
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
     * Recursively walk VAD tree (AVL tree of memory regions)
     *
     * @param vad_pa Physical address of MMVAD node
     * @param sections Output vector of memory sections
     */
    void WalkVADTree(uint64_t vad_pa, std::vector<MemorySection>& sections) {
        // Read MMVAD_SHORT structure (64 bytes minimum)
        // Layout: RTL_BALANCED_NODE (24 bytes) + StartingVpn (4) + EndingVpn (4) + ... + VadFlags (at 48)
        constexpr size_t MMVAD_READ_SIZE = 64;
        uint8_t vad_data[MMVAD_READ_SIZE];

        if (!ReadPhysicalMemory(vad_pa, vad_data, MMVAD_READ_SIZE)) {
            std::cerr << "[WalkVADTree] Failed to read VAD at PA 0x" << std::hex << vad_pa << std::dec << std::endl;
            return;
        }

        // Extract Left and Right child pointers (offsets 0 and 8 within RTL_BALANCED_NODE)
        uint64_t left_va = *reinterpret_cast<uint64_t*>(vad_data + 0);
        uint64_t right_va = *reinterpret_cast<uint64_t*>(vad_data + 8);

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
        section.type = MemorySection::UNKNOWN;  // TODO: Parse VadFlags to determine type
        section.name = "";  // TODO: Extract filename from MMVAD.Subsection if file-backed

        sections.push_back(section);

        // Recursively walk left child
        if (left_va != 0 && (left_va >> 48) == 0xffff) {
            // CRITICAL: Use System DTB for kernel VA translation
            uint64_t left_pa = TranslateVA(left_va, kernelInfo.swapper_pgd);
            if (left_pa != 0) {
                WalkVADTree(left_pa, sections);
            }
        }

        // Recursively walk right child
        if (right_va != 0 && (right_va >> 48) == 0xffff) {
            // CRITICAL: Use System DTB for kernel VA translation
            uint64_t right_pa = TranslateVA(right_va, kernelInfo.swapper_pgd);
            if (right_pa != 0) {
                WalkVADTree(right_pa, sections);
            }
        }
    }
};

} // namespace Haywire
