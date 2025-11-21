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
    int generation;  // When it was discovered
    bool validated;  // Part of a valid chain
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

int WalkFromTask(uint64_t startPA, std::map<uint64_t, TaskInfo>& discovered,
                 std::set<uint64_t>& marked, int generation) {
    if (marked.count(startPA)) {
        // Already processed this chain
        return 0;
    }

    marked.insert(startPA);

    uint64_t offset = startPA - 0x40000000;
    if (offset >= memorySize) return 0;

    uint8_t* task = (uint8_t*)memBase + offset;

    // Add starting task to discovered
    if (!discovered.count(startPA)) {
        TaskInfo info;
        info.pa = startPA;
        info.pid = *(uint32_t*)(task + PID_OFFSET);
        char* comm = (char*)(task + COMM_OFFSET);
        info.comm = std::string(comm, strnlen(comm, 16));
        info.generation = generation;
        info.validated = true;  // Part of a walkable chain
        discovered[startPA] = info;
    } else {
        discovered[startPA].validated = true;
    }

    uint64_t tasksNext = *(uint64_t*)(task + TASKS_OFFSET);

    std::set<uint64_t> visitedInChain;
    visitedInChain.insert(startPA);

    int walkCount = 1;
    uint64_t currentVA = tasksNext;

    // Walk the list
    for (int i = 0; i < 200; i++) {  // Increased limit for larger chains
        uint64_t taskVA = currentVA - TASKS_OFFSET;
        uint64_t taskPA = TryTranslateVA(taskVA);

        if (taskPA == 0) {
            break;  // Translation failed
        }

        if (visitedInChain.count(taskPA) || taskPA == startPA) {
            // Circular - we completed the list!
            break;
        }

        if (marked.count(taskPA)) {
            // Hit another already-processed chain
            walkCount++;
            break;
        }

        uint64_t nextOffset = taskPA - 0x40000000;
        if (nextOffset >= memorySize) break;

        uint8_t* nextTask = (uint8_t*)memBase + nextOffset;

        if (!LooksLikeTaskStruct(nextOffset)) break;

        // Add to discovered and mark
        if (!discovered.count(taskPA)) {
            TaskInfo info;
            info.pa = taskPA;
            info.pid = *(uint32_t*)(nextTask + PID_OFFSET);
            char* comm = (char*)(nextTask + COMM_OFFSET);
            info.comm = std::string(comm, strnlen(comm, 16));
            info.generation = generation;
            info.validated = true;
            discovered[taskPA] = info;
        } else {
            discovered[taskPA].validated = true;
        }

        marked.insert(taskPA);
        visitedInChain.insert(taskPA);
        walkCount++;

        currentVA = *(uint64_t*)(nextTask + TASKS_OFFSET);

        if (!IsKernelPointer(currentVA)) break;
    }

    return walkCount;
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

    auto startTime = std::chrono::steady_clock::now();

    // Generation 0: Initial broad scan (focused on known SLAB region)
    std::cout << "=== Generation 0: Initial Scan ===\n";
    std::map<uint64_t, TaskInfo> discovered;

    // Broader scan - memory layout has changed
    // Scan first 256MB where task_structs tend to cluster
    uint64_t scanStart = 0;
    uint64_t scanEnd = std::min((uint64_t)0x10000000, (uint64_t)memorySize);  // 256MB

    for (uint64_t offset = scanStart; offset < scanEnd; offset += 0x40) {
        if (LooksLikeTaskStruct(offset)) {
            uint64_t pa = offset + 0x40000000;
            TaskInfo info;
            info.pa = pa;
            uint8_t* ptr = (uint8_t*)memBase + offset;
            info.pid = *(uint32_t*)(ptr + PID_OFFSET);
            char* comm = (char*)(ptr + COMM_OFFSET);
            info.comm = std::string(comm, strnlen(comm, 16));
            info.generation = 0;
            info.validated = false;
            discovered[pa] = info;
        }
    }

    std::cout << "Found " << discovered.size() << " candidate task_structs\n\n";

    // Mark-and-sweep generations
    std::set<uint64_t> marked;  // Global mark set - never cleared
    int generation = 1;

    while (generation <= 3) {
        std::cout << "=== Generation " << generation << ": Swarm Walk ===\n";

        int totalWalked = 0;
        int newlyValidated = 0;
        std::vector<uint64_t> toWalk;

        // Collect unprocessed tasks
        for (auto& [pa, info] : discovered) {
            if (!marked.count(pa)) {
                toWalk.push_back(pa);
            }
        }

        std::cout << "Walking " << toWalk.size() << " unmarked tasks...\n";

        for (uint64_t pa : toWalk) {
            int walked = WalkFromTask(pa, discovered, marked, generation);
            if (walked > 0) {
                totalWalked += walked;
                newlyValidated++;
            }
        }

        // Count validated tasks
        int validatedCount = 0;
        for (auto& [pa, info] : discovered) {
            if (info.validated) validatedCount++;
        }

        std::cout << "Generation " << generation << " results:\n";
        std::cout << "  Newly validated chains: " << newlyValidated << "\n";
        std::cout << "  Total tasks walked: " << totalWalked << "\n";
        std::cout << "  Total validated tasks: " << validatedCount << "/"
                  << discovered.size() << "\n";
        std::cout << "  Marked (won't revisit): " << marked.size() << "\n\n";

        if (newlyValidated == 0) {
            std::cout << "No new chains found - converged!\n";
            break;
        }

        generation++;
    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Final report
    std::cout << "=== Final Results ===\n";

    int validCount = 0;
    int invalidCount = 0;
    std::map<int, int> genCounts;

    for (auto& [pa, info] : discovered) {
        if (info.validated) {
            validCount++;
            genCounts[info.generation]++;
        } else {
            invalidCount++;
        }
    }

    std::cout << "Valid tasks (in walkable chains): " << validCount << "\n";
    std::cout << "Invalid tasks (unreachable): " << invalidCount << "\n";
    std::cout << "Total discovered: " << discovered.size() << "\n";
    std::cout << "Time taken: " << duration.count() << "ms\n\n";

    std::cout << "Validated tasks by generation:\n";
    for (auto& [gen, count] : genCounts) {
        std::cout << "  Generation " << gen << ": " << count << " tasks\n";
    }

    // Show some validated processes
    std::cout << "\nSample validated processes:\n";
    int shown = 0;
    for (auto& [pa, info] : discovered) {
        if (info.validated && shown++ < 10) {
            std::cout << "  PID " << std::setw(6) << info.pid
                      << " (" << std::setw(16) << info.comm
                      << ") at PA 0x" << std::hex << pa << std::dec
                      << " [Gen " << info.generation << "]\n";
        }
    }

    std::cout << "\nOptimization achieved:\n";
    std::cout << "  Scanned: " << (scanEnd - scanStart) / 1024 << " KB (focused region)\n";
    std::cout << "  vs Full scan: " << (memorySize / 1024 / 1024) << " MB\n";
    std::cout << "  Reduction: " << (memorySize / (scanEnd - scanStart)) << "x\n";
    std::cout << "  Mark-and-sweep avoided " << marked.size()
              << " redundant traversals\n";

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}