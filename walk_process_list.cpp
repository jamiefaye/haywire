// Walk the kernel process list using QMP and physical memory access
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <vector>
#include <map>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// ARM64 Linux task_struct offsets (these vary by kernel version)
// We'll try multiple possible offsets
struct KernelOffsets {
    size_t pid;
    size_t comm;
    size_t tasks_next;
    size_t tasks_prev;
    size_t mm;
    size_t mm_pgd;
};

// Common offset configurations for different kernel versions
std::vector<KernelOffsets> OFFSET_CONFIGS = {
    // Linux 5.15+ common layout
    {0x4E8, 0x738, 0x3A0, 0x3A8, 0x520, 0x48},
    {0x4E0, 0x730, 0x398, 0x3A0, 0x518, 0x48},
    // Linux 5.10
    {0x398, 0x5C8, 0x2E0, 0x2E8, 0x3F0, 0x48},
    // Linux 5.4
    {0x3A0, 0x5D0, 0x2E8, 0x2F0, 0x3F8, 0x48},
    // Try some variations
    {0x500, 0x740, 0x3B0, 0x3B8, 0x530, 0x48},
};

class ProcessWalker {
public:
    ProcessWalker() : memFd(-1), memBase(nullptr), qmpSocket(-1) {}
    
    ~ProcessWalker() {
        Cleanup();
    }
    
    bool Initialize() {
        // Open physical memory
        memFd = open("/tmp/haywire-vm-mem", O_RDONLY);
        if (memFd < 0) {
            std::cerr << "Failed to open memory file\n";
            return false;
        }
        
        // Map 4GB of physical memory
        memBase = mmap(nullptr, MEMORY_SIZE, PROT_READ, MAP_SHARED, memFd, 0);
        if (memBase == MAP_FAILED) {
            std::cerr << "Failed to map memory\n";
            close(memFd);
            return false;
        }
        
        // Connect to QMP
        if (!ConnectQMP()) {
            std::cerr << "Failed to connect to QMP\n";
            return false;
        }
        
        return true;
    }
    
    void WalkProcesses() {
        std::cout << "\n=== Walking Process List via QMP + Physical Memory ===\n\n";
        
        // Step 1: Get kernel info including current task pointer
        json kernelInfo = GetKernelInfo();
        if (!kernelInfo.contains("current-task")) {
            std::cerr << "Could not get current task pointer\n";
            return;
        }
        
        uint64_t currentTaskVA = kernelInfo["current-task"].get<uint64_t>();
        uint64_t ttbr0 = kernelInfo["ttbr0"].get<uint64_t>();
        uint64_t ttbr1 = kernelInfo["ttbr1"].get<uint64_t>();
        
        std::cout << "Kernel Info:\n";
        std::cout << "  Current task VA: 0x" << std::hex << currentTaskVA << std::dec << "\n";
        std::cout << "  TTBR0: 0x" << std::hex << ttbr0 << std::dec << "\n";
        std::cout << "  TTBR1: 0x" << std::hex << ttbr1 << std::dec << "\n";
        
        // TTBR1 is the physical address of kernel page tables
        uint64_t kernel_pt_phys = ttbr1 & ~0xFFFULL;
        std::cout << "  Kernel PT physical: 0x" << std::hex << kernel_pt_phys << std::dec;
        
        // Check if this is within our mmap range (2GB)
        if (kernel_pt_phys < MEMORY_SIZE) {
            std::cout << " (ACCESSIBLE via mmap)\n";
        } else {
            std::cout << " (BEYOND mmap range - need QMP)\n";
        }
        std::cout << "\n";
        
        // Step 2: Translate current task VA to PA
        // Kernel addresses need TTBR1 for translation
        uint64_t currentTaskPA = TranslateVA2PA(currentTaskVA, ttbr1);
        if (!currentTaskPA) {
            std::cerr << "Could not translate current task address\n";
            std::cerr << "Trying without custom TTBR...\n";
            currentTaskPA = TranslateVA2PA(currentTaskVA);
            if (!currentTaskPA) {
                std::cerr << "Still failed\n";
                return;
            }
        }
        
        std::cout << "Current task PA: 0x" << std::hex << currentTaskPA << std::dec << "\n\n";
        
        // Step 3: Try to identify the correct offsets
        KernelOffsets* offsets = IdentifyOffsets(currentTaskPA);
        if (!offsets) {
            std::cerr << "Could not identify kernel structure offsets\n";
            return;
        }
        
        std::cout << "Identified offsets:\n";
        std::cout << "  PID: 0x" << std::hex << offsets->pid << std::dec << "\n";
        std::cout << "  COMM: 0x" << std::hex << offsets->comm << std::dec << "\n";
        std::cout << "  tasks.next: 0x" << std::hex << offsets->tasks_next << std::dec << "\n\n";
        
        // Step 4: Walk the process list
        std::cout << "Process List:\n";
        std::cout << "PID    | Name             | TTBR              | Status\n";
        std::cout << "-------|------------------|-------------------|-------\n";
        
        WalkTaskList(currentTaskPA, *offsets, ttbr1);
    }
    
private:
    static constexpr size_t MEMORY_SIZE = 4ULL * 1024 * 1024 * 1024; // 4GB
    int memFd;
    void* memBase;
    int qmpSocket;
    
    bool ConnectQMP() {
        qmpSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (qmpSocket < 0) return false;
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(4445);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        if (connect(qmpSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(qmpSocket);
            return false;
        }
        
        // Read greeting
        char buffer[4096];
        recv(qmpSocket, buffer, sizeof(buffer), 0);
        
        // Enter command mode
        std::string cmd = "{\"execute\": \"qmp_capabilities\"}\n";
        send(qmpSocket, cmd.c_str(), cmd.length(), 0);
        recv(qmpSocket, buffer, sizeof(buffer), 0);
        
        return true;
    }
    
    json GetKernelInfo() {
        json cmd = {
            {"execute", "query-kernel-info"},
            {"arguments", {{"cpu-index", 0}}}
        };
        
        std::string cmdStr = cmd.dump() + "\n";
        send(qmpSocket, cmdStr.c_str(), cmdStr.length(), 0);
        
        char buffer[4096];
        int received = recv(qmpSocket, buffer, sizeof(buffer)-1, 0);
        if (received <= 0) return json();
        
        buffer[received] = '\0';
        
        try {
            json response = json::parse(buffer);
            if (response.contains("return")) {
                return response["return"];
            }
        } catch (...) {}
        
        return json();
    }
    
    uint64_t TranslateVA2PA(uint64_t va, uint64_t ttbr = 0) {
        json cmd = {
            {"execute", "query-va2pa"},
            {"arguments", {
                {"cpu-index", 0},
                {"addr", va}
            }}
        };
        
        // Add TTBR if provided
        if (ttbr) {
            cmd["arguments"]["ttbr"] = ttbr;
        }
        
        std::string cmdStr = cmd.dump() + "\n";
        send(qmpSocket, cmdStr.c_str(), cmdStr.length(), 0);
        
        char buffer[4096];
        int received = recv(qmpSocket, buffer, sizeof(buffer)-1, 0);
        if (received <= 0) return 0;
        
        buffer[received] = '\0';
        
        try {
            json response = json::parse(buffer);
            if (response.contains("return")) {
                auto ret = response["return"];
                if (ret["valid"].get<bool>()) {
                    return ret["phys"].get<uint64_t>();
                }
            }
        } catch (...) {}
        
        return 0;
    }
    
    KernelOffsets* IdentifyOffsets(uint64_t taskPA) {
        if (taskPA >= MEMORY_SIZE) return nullptr;
        
        uint8_t* task = (uint8_t*)memBase + taskPA;
        
        // Try each offset configuration
        for (auto& offsets : OFFSET_CONFIGS) {
            // Sanity checks
            if (offsets.comm + 16 > 0x1000) continue;  // Would be past page
            if (offsets.pid + 4 > 0x1000) continue;
            
            // Check if comm field looks like a valid process name
            char* comm = (char*)(task + offsets.comm);
            bool validComm = true;
            bool hasChar = false;
            
            for (int i = 0; i < 16; i++) {
                if (comm[i] == 0) break;
                if (comm[i] < 32 || comm[i] > 126) {
                    validComm = false;
                    break;
                }
                hasChar = true;
            }
            
            if (validComm && hasChar) {
                // Check if PID looks reasonable (0-100000)
                int32_t pid = *(int32_t*)(task + offsets.pid);
                if (pid >= 0 && pid < 100000) {
                    std::cout << "Found valid offsets with comm='" << comm 
                              << "' pid=" << pid << "\n";
                    return &offsets;
                }
            }
        }
        
        return nullptr;
    }
    
    void WalkTaskList(uint64_t startTaskPA, const KernelOffsets& offsets, uint64_t ttbr1) {
        std::vector<uint64_t> visited;
        uint64_t currentPA = startTaskPA;
        int count = 0;
        int maxProcesses = 100;
        
        while (count < maxProcesses) {
            // Check if we've seen this address (loop detection)
            if (std::find(visited.begin(), visited.end(), currentPA) != visited.end()) {
                break;
            }
            visited.push_back(currentPA);
            
            // Print process info
            if (!PrintTaskInfo(currentPA, offsets, ttbr1)) {
                break;
            }
            count++;
            
            // Get next task
            uint64_t nextVA = GetNextTask(currentPA, offsets);
            if (!nextVA) break;
            
            // Translate to physical (kernel VA needs TTBR1)
            uint64_t nextPA = TranslateVA2PA(nextVA, ttbr1);
            if (!nextPA || nextPA == startTaskPA) {
                break;
            }
            
            currentPA = nextPA;
        }
        
        std::cout << "\nTotal processes found: " << count << "\n";
    }
    
    uint64_t GetNextTask(uint64_t taskPA, const KernelOffsets& offsets) {
        if (taskPA >= MEMORY_SIZE) return 0;
        
        uint8_t* task = (uint8_t*)memBase + taskPA;
        
        // Read tasks.next pointer
        uint64_t nextVA = *(uint64_t*)(task + offsets.tasks_next);
        
        // tasks.next points to the tasks member of next task_struct
        // Subtract offset to get start of task_struct
        nextVA -= offsets.tasks_next;
        
        return nextVA;
    }
    
    bool PrintTaskInfo(uint64_t taskPA, const KernelOffsets& offsets, uint64_t ttbr1) {
        if (taskPA >= MEMORY_SIZE) return false;
        
        uint8_t* task = (uint8_t*)memBase + taskPA;
        
        // Read PID
        int32_t pid = *(int32_t*)(task + offsets.pid);
        
        // Read process name
        char comm[17] = {0};
        memcpy(comm, task + offsets.comm, 16);
        
        // Read mm pointer
        uint64_t mmVA = *(uint64_t*)(task + offsets.mm);
        
        // Get TTBR if process has mm
        uint64_t ttbr = 0;
        if (mmVA) {
            uint64_t mmPA = TranslateVA2PA(mmVA, ttbr1);
            if (mmPA && mmPA < MEMORY_SIZE) {
                uint8_t* mm = (uint8_t*)memBase + mmPA;
                uint64_t pgd = *(uint64_t*)(mm + offsets.mm_pgd);
                // PGD is a kernel VA, translate to PA for TTBR
                ttbr = TranslateVA2PA(pgd, ttbr1);
            }
        }
        
        // Print info
        std::cout << std::setw(6) << pid << " | ";
        std::cout << std::setw(16) << std::left << comm << " | ";
        if (ttbr) {
            std::cout << "0x" << std::hex << std::setw(16) << std::setfill('0') 
                      << ttbr << std::dec << std::setfill(' ') << " | ";
            std::cout << "user";
        } else {
            std::cout << std::setw(18) << "-" << " | ";
            std::cout << "kernel";
        }
        std::cout << "\n";
        
        return true;
    }
    
    void Cleanup() {
        if (memBase && memBase != MAP_FAILED) {
            munmap(memBase, MEMORY_SIZE);
        }
        if (memFd >= 0) {
            close(memFd);
        }
        if (qmpSocket >= 0) {
            close(qmpSocket);
        }
    }
};

int main() {
    ProcessWalker walker;
    
    if (!walker.Initialize()) {
        std::cerr << "Failed to initialize\n";
        return 1;
    }
    
    walker.WalkProcesses();
    
    return 0;
}