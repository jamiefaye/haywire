// Proof of concept: Find and list all processes using QMP + physical memory
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// Ubuntu 22.04 ARM64 kernel structure offsets (approximate - may need tuning)
// These are typical offsets for 5.15-6.x kernels
namespace KernelOffsets {
    // task_struct offsets
    constexpr size_t TASK_PID = 0x4E8;        // pid field
    constexpr size_t TASK_COMM = 0x738;       // comm[16] - process name
    constexpr size_t TASK_TASKS_NEXT = 0x3A0; // tasks.next - next process
    constexpr size_t TASK_MM = 0x520;         // mm pointer
    
    // mm_struct offsets  
    constexpr size_t MM_PGD = 0x48;           // pgd - page table base
}

class ProcessFinder {
public:
    ProcessFinder() : memFd(-1), memBase(nullptr), qmpSocket(-1) {}
    
    ~ProcessFinder() {
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
    
    void FindProcesses() {
        std::cout << "\n=== Finding processes via QMP + physical memory ===\n\n";
        
        // Step 1: Find init_task using known kernel addresses
        uint64_t initTaskVA = FindInitTask();
        if (!initTaskVA) {
            std::cerr << "Could not find init_task\n";
            return;
        }
        
        std::cout << "Found init_task at VA: 0x" << std::hex << initTaskVA << std::dec << "\n\n";
        
        // Step 2: Translate to physical address
        uint64_t initTaskPA = TranslateVA2PA(initTaskVA);
        if (!initTaskPA) {
            std::cerr << "Could not translate init_task address\n";
            return;
        }
        
        std::cout << "init_task physical address: 0x" << std::hex << initTaskPA << std::dec << "\n\n";
        
        // Step 3: Walk the process list
        std::cout << "Process List:\n";
        std::cout << "----------------------------------------\n";
        WalkProcessList(initTaskPA);
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
        addr.sin_port = htons(4445);  // QMP port (from launch script)
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
    
    uint64_t FindInitTask() {
        // Scan broader kernel data section range
        // Most kernels have init_task in the data section
        std::cout << "Scanning kernel memory for init_task (this may take a moment)...\n";
        
        // Scan kernel data sections - adjust range for different kernels
        // Start with common ranges for ARM64 Linux
        std::vector<std::pair<uint64_t, uint64_t>> ranges = {
            {0xffff000010000000, 0xffff000012000000},  // Kernel data section (correct range!)
            {0xffff000008000000, 0xffff00000A000000},  // Kernel text (might have init_task)
        };
        
        int pagesChecked = 0;
        for (auto& range : ranges) {
            std::cout << "Scanning range 0x" << std::hex << range.first 
                      << " - 0x" << range.second << std::dec << "\n";
                      
            for (uint64_t va = range.first; va < range.second; va += 0x1000) {
                uint64_t pa = TranslateVA2PA(va);
                pagesChecked++;
                
                // Show progress
                if (pagesChecked % 100 == 0) {
                    std::cout << "." << std::flush;
                }
                
                if (pa && pa < MEMORY_SIZE) {
                    // Check multiple offsets within the page
                    for (size_t offset = 0; offset < 0x1000; offset += 0x100) {
                        if (IsLikelyInitTask(pa + offset)) {
                            std::cout << "\nFound potential init_task at VA: 0x" 
                                      << std::hex << (va + offset) << std::dec << "\n";
                            return va + offset;
                        }
                    }
                }
            }
            std::cout << "\n";
        }
        
        return 0;
    }
    
    uint64_t TranslateVA2PA(uint64_t va) {
        json cmd = {
            {"execute", "query-va2pa"},
            {"arguments", {
                {"cpu-index", 0},
                {"addr", va}
            }}
        };
        
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
    
    bool IsLikelyTaskStruct(uint64_t pa) {
        if (pa >= MEMORY_SIZE) return false;
        
        // Check for task_struct patterns
        uint8_t* ptr = (uint8_t*)memBase + pa;
        
        // Check comm field - should be "swapper/0" for init
        char* comm = (char*)(ptr + KernelOffsets::TASK_COMM);
        if (strncmp(comm, "swapper", 7) == 0) {
            return true;
        }
        
        return false;
    }
    
    bool IsLikelyInitTask(uint64_t pa) {
        if (pa >= MEMORY_SIZE) return false;
        
        uint8_t* ptr = (uint8_t*)memBase + pa;
        
        // Try different possible offsets for PID and comm
        // These vary between kernel versions
        std::vector<std::pair<size_t, size_t>> offsetPairs = {
            {0x4E8, 0x738},  // Common 5.15+
            {0x4E0, 0x730},  // Alternative
            {0x398, 0x5C8},  // Older kernels  
            {0x3A0, 0x5D0},  // Another variant
            {0x500, 0x740},  // Yet another
        };
        
        for (auto& offsets : offsetPairs) {
            if (offsets.first + 4 >= 0x1000 || offsets.second + 16 >= 0x1000) {
                continue; // Would read past our buffer
            }
            
            uint32_t pid = *(uint32_t*)(ptr + offsets.first);
            char* comm = (char*)(ptr + offsets.second);
            
            // init_task has PID 0 and name starting with "swapper"
            if (pid == 0) {
                // Check if comm looks like a valid string
                bool validString = true;
                for (int i = 0; i < 16; i++) {
                    if (comm[i] == 0) break;
                    if (comm[i] < 32 || comm[i] > 126) {
                        validString = false;
                        break;
                    }
                }
                
                if (validString && strncmp(comm, "swapper", 7) == 0) {
                    std::cout << "\nFound with offsets: PID=0x" << std::hex << offsets.first 
                              << " COMM=0x" << offsets.second << std::dec << "\n";
                    // Update the global offsets
                    const_cast<size_t&>(KernelOffsets::TASK_PID) = offsets.first;
                    const_cast<size_t&>(KernelOffsets::TASK_COMM) = offsets.second;
                    return true;
                }
            }
        }
        
        return false;
    }
    
    void WalkProcessList(uint64_t initTaskPA) {
        std::vector<uint64_t> visited;
        uint64_t currentPA = initTaskPA;
        int count = 0;
        int maxProcesses = 1000;  // Safety limit
        
        while (count < maxProcesses) {
            // Check if we've seen this address before (loop detection)
            if (std::find(visited.begin(), visited.end(), currentPA) != visited.end()) {
                if (count > 0) {
                    std::cout << "\nFound " << count << " processes\n";
                    return;
                }
                break;
            }
            visited.push_back(currentPA);
            
            // Read process info
            if (!PrintProcessInfo(currentPA, count)) {
                break;
            }
            count++;
            
            // Get next process
            uint64_t nextPA = GetNextProcess(currentPA);
            if (!nextPA || nextPA == initTaskPA) {
                std::cout << "\nFound " << count << " processes\n";
                return;
            }
            
            currentPA = nextPA;
        }
    }
    
    bool PrintProcessInfo(uint64_t taskPA, int index) {
        if (taskPA >= MEMORY_SIZE) return false;
        
        uint8_t* task = (uint8_t*)memBase + taskPA;
        
        // Read PID
        int32_t pid = *(int32_t*)(task + KernelOffsets::TASK_PID);
        
        // Read process name (comm field)
        char comm[17] = {0};
        memcpy(comm, task + KernelOffsets::TASK_COMM, 16);
        comm[16] = '\0';
        
        // Read mm pointer (will be NULL for kernel threads)
        uint64_t mm = *(uint64_t*)(task + KernelOffsets::TASK_MM);
        
        // Read TTBR if process has memory
        uint64_t ttbr = 0;
        if (mm) {
            // mm is a kernel virtual address, need to translate it
            uint64_t mmPA = TranslateVA2PA(mm);
            if (mmPA && mmPA < MEMORY_SIZE) {
                uint8_t* mmStruct = (uint8_t*)memBase + mmPA;
                ttbr = *(uint64_t*)(mmStruct + KernelOffsets::MM_PGD);
            }
        }
        
        // Print process info
        std::cout << std::setw(5) << pid << " | ";
        std::cout << std::setw(16) << std::left << comm << " | ";
        if (ttbr) {
            std::cout << "TTBR: 0x" << std::hex << ttbr << std::dec;
        } else {
            std::cout << "kernel thread";
        }
        std::cout << "\n";
        
        return true;
    }
    
    uint64_t GetNextProcess(uint64_t taskPA) {
        if (taskPA >= MEMORY_SIZE) return 0;
        
        uint8_t* task = (uint8_t*)memBase + taskPA;
        
        // Read tasks.next pointer (kernel virtual address)
        uint64_t nextVA = *(uint64_t*)(task + KernelOffsets::TASK_TASKS_NEXT);
        
        // tasks.next points to the tasks member of the next task_struct
        // We need to subtract the offset to get the start of task_struct
        nextVA -= KernelOffsets::TASK_TASKS_NEXT;
        
        // Translate to physical
        return TranslateVA2PA(nextVA);
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
    ProcessFinder finder;
    
    if (!finder.Initialize()) {
        std::cerr << "Failed to initialize\n";
        return 1;
    }
    
    finder.FindProcesses();
    
    return 0;
}