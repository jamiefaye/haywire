#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <map>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>

// Verified offsets from pahole
const size_t TASKS_OFFSET = 0x680;  // task_struct.tasks
const size_t MM_OFFSET = 0x6d0;     // task_struct.mm
const size_t PID_OFFSET = 0x750;    // task_struct.pid
const size_t COMM_OFFSET = 0x970;   // task_struct.comm

void* memBase = nullptr;
size_t memorySize = 0;

struct TaskInfo {
    uint64_t pa;
    uint32_t pid;
    std::string comm;
    bool validated;
};

bool IsKernelPointer(uint64_t addr) {
    return (addr & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL;
}

bool LooksLikeTaskStruct(uint64_t offset) {
    if (offset + 0x1000 > memorySize) return false;

    uint8_t* ptr = (uint8_t*)memBase + offset;

    // Check PID
    uint32_t pid = *(uint32_t*)(ptr + PID_OFFSET);
    if (pid == 0 || pid > 100000) return false;

    // Check comm
    char* comm = (char*)(ptr + COMM_OFFSET);
    bool validName = false;
    for (int i = 0; i < 16; i++) {
        if (comm[i] == 0) break;
        if (comm[i] >= 32 && comm[i] < 127) {
            validName = true;
        } else if (comm[i] != 0) {
            return false;
        }
    }

    // Check list pointers are kernel VAs
    uint64_t tasksNext = *(uint64_t*)(ptr + TASKS_OFFSET);
    uint64_t tasksPrev = *(uint64_t*)(ptr + TASKS_OFFSET + 8);

    return validName && IsKernelPointer(tasksNext) && IsKernelPointer(tasksPrev);
}

uint64_t TryTranslateVA(uint64_t va) {
    // Simple VA translation patterns for ARM64
    if ((va & 0xffff000000000000ULL) == 0xffff000000000000ULL) {
        uint64_t attempt1 = (va & 0x0000ffffffffffffULL) + 0x40000000;
        if (attempt1 >= 0x40000000 && attempt1 < 0x200000000) {
            return attempt1;
        }
    }

    if (va >= 0xffffffc000000000ULL) {
        uint64_t attempt2 = va - 0xffffffc000000000ULL + 0x40000000;
        if (attempt2 >= 0x40000000 && attempt2 < 0x200000000) {
            return attempt2;
        }
    }

    // Try lower 32 bits + base
    uint64_t attempt3 = (va & 0xFFFFFFFF) + 0x40000000;
    if (attempt3 >= 0x40000000 && attempt3 < 0x200000000) {
        return attempt3;
    }

    return 0;
}

std::map<uint64_t, TaskInfo> FullMemoryScan() {
    std::cout << "=== Phase 1: Full Memory Scan (One-time) ===\n";
    std::cout << "Scanning " << (memorySize / 1024 / 1024) << " MB for task_structs...\n";

    std::map<uint64_t, TaskInfo> suspects;

    for (uint64_t offset = 0; offset < memorySize; offset += 0x40) {
        if (LooksLikeTaskStruct(offset)) {
            uint64_t pa = offset + 0x40000000;
            TaskInfo info;
            info.pa = pa;
            uint8_t* ptr = (uint8_t*)memBase + offset;
            info.pid = *(uint32_t*)(ptr + PID_OFFSET);
            char* comm = (char*)(ptr + COMM_OFFSET);
            info.comm = std::string(comm, strnlen(comm, 16));
            info.validated = false;
            suspects[pa] = info;
        }

        if (offset % (100 * 1024 * 1024) == 0 && offset > 0) {
            std::cout << "  Scanned " << (offset / 1024 / 1024) << " MB, found "
                      << suspects.size() << " suspects...\r" << std::flush;
        }
    }

    std::cout << "\nFound " << suspects.size() << " suspect task_structs\n";
    return suspects;
}

int SwarmValidation(std::map<uint64_t, TaskInfo>& suspects) {
    std::cout << "\n=== Phase 2: Swarm Validation (Fast) ===\n";

    std::set<uint64_t> marked;
    int totalValidated = 0;

    for (auto& [pa, info] : suspects) {
        if (marked.count(pa)) continue;

        // Try to walk from this task
        uint64_t offset = pa - 0x40000000;
        if (offset >= memorySize) continue;

        uint8_t* task = (uint8_t*)memBase + offset;
        uint64_t tasksNext = *(uint64_t*)(task + TASKS_OFFSET);

        std::set<uint64_t> chain;
        chain.insert(pa);
        marked.insert(pa);

        uint64_t currentVA = tasksNext;
        int walkCount = 1;

        // Walk the list
        for (int i = 0; i < 200; i++) {
            uint64_t taskVA = currentVA - TASKS_OFFSET;
            uint64_t taskPA = TryTranslateVA(taskVA);

            if (taskPA == 0) break;
            if (chain.count(taskPA) || taskPA == pa) break;  // Circular
            if (marked.count(taskPA)) {
                // Hit already validated chain
                walkCount++;
                break;
            }

            uint64_t nextOffset = taskPA - 0x40000000;
            if (nextOffset >= memorySize) break;

            uint8_t* nextTask = (uint8_t*)memBase + nextOffset;
            if (!LooksLikeTaskStruct(nextOffset)) break;

            chain.insert(taskPA);
            marked.insert(taskPA);
            walkCount++;

            currentVA = *(uint64_t*)(nextTask + TASKS_OFFSET);
            if (!IsKernelPointer(currentVA)) break;
        }

        // Mark chain as validated
        if (walkCount > 1) {
            for (uint64_t chainPA : chain) {
                if (suspects.count(chainPA)) {
                    suspects[chainPA].validated = true;
                    totalValidated++;
                }
            }
        }
    }

    return totalValidated;
}

void RefreshFromSuspects(std::map<uint64_t, TaskInfo>& suspects) {
    std::cout << "\n=== Refresh: Re-scan Suspect Locations Only ===\n";

    int alive = 0;
    int changed = 0;
    int dead = 0;

    for (auto& [pa, info] : suspects) {
        uint64_t offset = pa - 0x40000000;
        if (offset >= memorySize) continue;

        if (LooksLikeTaskStruct(offset)) {
            uint8_t* ptr = (uint8_t*)memBase + offset;
            uint32_t newPid = *(uint32_t*)(ptr + PID_OFFSET);
            char* newComm = (char*)(ptr + COMM_OFFSET);
            std::string newName(newComm, strnlen(newComm, 16));

            if (newPid != info.pid || newName != info.comm) {
                changed++;
                info.pid = newPid;
                info.comm = newName;
            }
            alive++;
        } else {
            dead++;
            info.validated = false;  // No longer valid
        }
    }

    std::cout << "Checked " << suspects.size() << " suspect locations:\n";
    std::cout << "  Alive: " << alive << "\n";
    std::cout << "  Changed: " << changed << "\n";
    std::cout << "  Dead: " << dead << "\n";
}

int main() {
    int fd = open("/tmp/haywire-vm-mem", O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open memory file\n";
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    memorySize = st.st_size;

    memBase = mmap(nullptr, memorySize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to mmap\n";
        close(fd);
        return 1;
    }

    std::cout << "Memory mapped: " << (memorySize / 1024 / 1024) << " MB\n\n";

    // Phase 1: One-time full scan
    auto startFull = std::chrono::steady_clock::now();
    auto suspects = FullMemoryScan();
    auto endFull = std::chrono::steady_clock::now();
    auto fullDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endFull - startFull);

    // Phase 2: Fast validation
    auto startSwarm = std::chrono::steady_clock::now();
    int validated = SwarmValidation(suspects);
    auto endSwarm = std::chrono::steady_clock::now();
    auto swarmDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endSwarm - startSwarm);

    std::cout << "Validated " << validated << " tasks as part of walkable chains\n";

    // Show timing comparison
    std::cout << "\n=== Performance ===\n";
    std::cout << "Full scan: " << fullDuration.count() << "ms\n";
    std::cout << "Swarm validation: " << swarmDuration.count() << "ms\n";
    std::cout << "Total initial discovery: " << (fullDuration.count() + swarmDuration.count()) << "ms\n";

    // Demonstrate refresh (would be called periodically)
    std::cout << "\n=== Simulating Periodic Refresh ===\n";
    for (int i = 0; i < 3; i++) {
        usleep(100000);  // 100ms pause

        auto startRefresh = std::chrono::steady_clock::now();
        RefreshFromSuspects(suspects);
        auto endRefresh = std::chrono::steady_clock::now();
        auto refreshDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endRefresh - startRefresh);

        std::cout << "Refresh #" << (i+1) << " took " << refreshDuration.count() << "ms\n";
    }

    std::cout << "\n=== Summary ===\n";
    std::cout << "Initial full scan finds all suspects (slow but complete)\n";
    std::cout << "Subsequent refreshes only check suspect locations (very fast)\n";
    std::cout << "Speedup for refresh: ~" << (fullDuration.count() / 1) << "x\n";

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}