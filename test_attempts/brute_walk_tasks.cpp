#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// Verified offsets from pahole
const size_t TASKS_OFFSET = 0x680;  // task_struct.tasks
const size_t MM_OFFSET = 0x6d0;     // task_struct.mm
const size_t PID_OFFSET = 0x750;    // task_struct.pid
const size_t COMM_OFFSET = 0x970;   // task_struct.comm

void* memBase = nullptr;
size_t memorySize = 0;
const uint64_t SWAPPER_PGD = 0x136deb000;

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

// Simple VA translation - just for the linear mapping
uint64_t TryTranslateVA(uint64_t va) {
    // Common linear mapping patterns for ARM64
    // Try pattern 1: VA = PA + 0xffff000000000000 - 0x40000000
    if ((va & 0xffff000000000000ULL) == 0xffff000000000000ULL) {
        uint64_t attempt1 = (va & 0x0000ffffffffffffULL) + 0x40000000;
        if (attempt1 >= 0x40000000 && attempt1 < 0x200000000) {
            return attempt1;
        }
    }

    // Try pattern 2: Direct offset mapping
    if (va >= 0xffffffc000000000ULL) {
        uint64_t attempt2 = va - 0xffffffc000000000ULL + 0x40000000;
        if (attempt2 >= 0x40000000 && attempt2 < 0x200000000) {
            return attempt2;
        }
    }

    return 0;
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

    // First, find ALL potential task_structs
    std::cout << "=== Phase 1: Finding ALL Task_structs ===\n";
    std::vector<uint64_t> candidatePAs;

    // Focus on the known SLAB region (adjust based on your system)
    uint64_t scanStart = 0;  // Start from beginning
    uint64_t scanEnd = memorySize;  // Full scan

    for (uint64_t offset = scanStart; offset < scanEnd; offset += 0x40) {  // 64-byte aligned
        if (LooksLikeTaskStruct(offset)) {
            uint64_t pa = offset + 0x40000000;
            candidatePAs.push_back(pa);

            if (candidatePAs.size() <= 10) {  // Show first few
                uint8_t* ptr = (uint8_t*)memBase + offset;
                uint32_t pid = *(uint32_t*)(ptr + PID_OFFSET);
                char* comm = (char*)(ptr + COMM_OFFSET);
                std::cout << "  Found PID " << pid << " ("
                          << std::string(comm, strnlen(comm, 16))
                          << ") at PA 0x" << std::hex << pa << std::dec << "\n";
            }
        }

        if (offset % (100 * 1024 * 1024) == 0 && offset > 0) {
            std::cout << "  Scanned " << (offset / 1024 / 1024) << " MB, found "
                      << candidatePAs.size() << " candidates...\r" << std::flush;
        }
    }

    std::cout << "\nFound " << candidatePAs.size() << " potential task_structs\n\n";

    // Now try to walk from EACH one
    std::cout << "=== Phase 2: Attempting List Walk from Each Task ===\n";

    int maxWalked = 0;
    uint64_t bestStartPA = 0;

    for (uint64_t startPA : candidatePAs) {
        uint64_t offset = startPA - 0x40000000;
        uint8_t* task = (uint8_t*)memBase + offset;

        uint64_t tasksNext = *(uint64_t*)(task + TASKS_OFFSET);

        // Try to walk the list
        std::set<uint64_t> visited;
        visited.insert(startPA);

        int walkCount = 0;
        uint64_t currentVA = tasksNext;
        bool failed = false;

        // Try to walk up to 100 tasks
        for (int i = 0; i < 100; i++) {
            // tasks.next points to the tasks field of next task
            uint64_t taskVA = currentVA - TASKS_OFFSET;

            // Try simple VA translation patterns
            uint64_t taskPA = TryTranslateVA(taskVA);
            if (taskPA == 0) {
                // Try interpreting the VA differently
                // Sometimes the lower bits are the offset
                taskPA = (currentVA & 0xFFFFFFFF) + 0x40000000;
                if (taskPA < 0x40000000 || taskPA >= 0x200000000) {
                    failed = true;
                    break;
                }
            }

            if (visited.count(taskPA)) {
                // Circular - we completed the list!
                break;
            }

            uint64_t nextOffset = taskPA - 0x40000000;
            if (nextOffset >= memorySize) {
                failed = true;
                break;
            }

            uint8_t* nextTask = (uint8_t*)memBase + nextOffset;

            // Validate it looks like a task
            if (!LooksLikeTaskStruct(nextOffset)) {
                failed = true;
                break;
            }

            visited.insert(taskPA);
            walkCount++;

            // Get next pointer
            currentVA = *(uint64_t*)(nextTask + TASKS_OFFSET);

            if (!IsKernelPointer(currentVA)) {
                failed = true;
                break;
            }
        }

        if (walkCount > maxWalked) {
            maxWalked = walkCount;
            bestStartPA = startPA;

            if (walkCount > 10) {  // Significant chain found!
                uint32_t pid = *(uint32_t*)(task + PID_OFFSET);
                char* comm = (char*)(task + COMM_OFFSET);

                std::cout << "✓ From PID " << pid << " ("
                          << std::string(comm, strnlen(comm, 16))
                          << ") at PA 0x" << std::hex << startPA << std::dec
                          << " - walked " << walkCount << " tasks!\n";
            }
        }
    }

    std::cout << "\n=== Results ===\n";
    if (maxWalked > 10) {
        std::cout << "SUCCESS! Best chain: " << maxWalked << " tasks from PA 0x"
                  << std::hex << bestStartPA << std::dec << "\n";
        std::cout << "This proves the linked list IS traversable!\n";
        std::cout << "We just need better VA->PA translation.\n";
    } else if (maxWalked > 0) {
        std::cout << "Partial success: walked " << maxWalked << " tasks\n";
        std::cout << "List exists but our VA translation is incomplete.\n";
    } else {
        std::cout << "Could not walk any significant chains.\n";
        std::cout << "Either:\n";
        std::cout << "1. VA translation pattern not figured out\n";
        std::cout << "2. Memory is too fragmented/stale\n";
        std::cout << "3. Need to catch memory at the right moment\n";
    }

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}