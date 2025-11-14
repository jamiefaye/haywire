#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
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

uint64_t TranslateVA(uint64_t va, uint64_t pgdBase) {
    // Simplified translation - just checking if we can translate
    // In reality would need full page walk
    return 0;  // For now, we'll work with PAs directly
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

    // Start with known good task_struct from our scan
    uint64_t startPA = 0x42238000;  // PID 1650 (gnome-shell-cal)
    std::cout << "Starting from known task_struct at PA 0x"
              << std::hex << startPA << std::dec << "\n";

    uint64_t offset = startPA - 0x40000000;
    if (offset >= memorySize) {
        std::cerr << "Start PA out of bounds\n";
        return 1;
    }

    uint8_t* task = (uint8_t*)memBase + offset;

    // Read initial task
    uint32_t pid = *(uint32_t*)(task + PID_OFFSET);
    char* comm = (char*)(task + COMM_OFFSET);
    uint64_t tasksNext = *(uint64_t*)(task + TASKS_OFFSET);
    uint64_t tasksPrev = *(uint64_t*)(task + TASKS_OFFSET + 8);

    std::cout << "\nInitial task:\n";
    std::cout << "  PID: " << pid << "\n";
    std::cout << "  Name: " << std::string(comm, strnlen(comm, 16)) << "\n";
    std::cout << "  tasks.next: 0x" << std::hex << tasksNext << std::dec << "\n";
    std::cout << "  tasks.prev: 0x" << std::hex << tasksPrev << std::dec << "\n";

    // Check if next/prev are kernel pointers
    if (!IsKernelPointer(tasksNext)) {
        std::cerr << "\ntasks.next is not a kernel pointer!\n";
        std::cerr << "Cannot walk list from this task\n";
        return 1;
    }

    std::cout << "\n=== Attempting to Walk Process List ===\n";
    std::cout << "Note: This will fail if we can't translate VAs\n\n";

    // Try to walk the list
    std::set<uint64_t> visited;
    visited.insert(startPA);

    // The tasks.next pointer points to the tasks field of the next task
    // We need to subtract TASKS_OFFSET to get the task_struct address
    uint64_t nextTaskVA = tasksNext - TASKS_OFFSET;

    std::cout << "Next task VA would be: 0x" << std::hex << nextTaskVA << std::dec << "\n";

    if (IsKernelPointer(nextTaskVA)) {
        std::cout << "This is a kernel VA that we'd need to translate\n";

        // Since we can't translate VAs easily here, let's try another approach
        // Let's scan nearby memory for other task_structs and check if they link back

        std::cout << "\n=== Checking Nearby Task_structs for Links ===\n";

        // Scan within 1MB of our known task
        for (uint64_t scanPA = startPA - 0x100000; scanPA < startPA + 0x100000; scanPA += 0x1000) {
            if (scanPA < 0x40000000 || scanPA >= 0x40000000 + memorySize) continue;
            if (scanPA == startPA) continue;

            uint64_t scanOffset = scanPA - 0x40000000;
            uint8_t* scanTask = (uint8_t*)memBase + scanOffset;

            // Quick check if it looks like a task_struct
            uint32_t scanPid = *(uint32_t*)(scanTask + PID_OFFSET);
            if (scanPid > 0 && scanPid < 100000) {
                uint64_t scanNext = *(uint64_t*)(scanTask + TASKS_OFFSET);
                uint64_t scanPrev = *(uint64_t*)(scanTask + TASKS_OFFSET + 8);

                // Check if this task's prev or next points back to our original task
                // (Would need VA translation to verify properly)
                char* scanComm = (char*)(scanTask + COMM_OFFSET);
                bool validName = false;
                for (int i = 0; i < 16 && scanComm[i]; i++) {
                    if (scanComm[i] >= 32 && scanComm[i] < 127) {
                        validName = true;
                    }
                }

                if (validName && IsKernelPointer(scanNext) && IsKernelPointer(scanPrev)) {
                    std::cout << "Found nearby task at PA 0x" << std::hex << scanPA << std::dec
                              << " PID=" << scanPid
                              << " Name=" << std::string(scanComm, strnlen(scanComm, 16)) << "\n";
                    std::cout << "  Its tasks.next: 0x" << std::hex << scanNext << std::dec << "\n";
                    std::cout << "  Its tasks.prev: 0x" << std::hex << scanPrev << std::dec << "\n";

                    // In a complete implementation, we'd translate these VAs and check
                    // if they point back to our original task
                }
            }
        }
    }

    std::cout << "\n=== Summary ===\n";
    std::cout << "We found valid task_structs with kernel VA pointers in tasks list\n";
    std::cout << "To walk the list, we need to:\n";
    std::cout << "1. Translate the VA (tasks.next) to PA\n";
    std::cout << "2. Subtract TASKS_OFFSET to get task_struct address\n";
    std::cout << "3. Read that task and repeat\n";
    std::cout << "\nThe issue is VA translation is failing for these addresses\n";
    std::cout << "Even though the task_structs themselves are valid!\n";

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}