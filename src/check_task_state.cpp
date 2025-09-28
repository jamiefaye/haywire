/**
 * Check task state to identify live vs dead processes
 */

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <map>
#include <set>

// Task states from Linux kernel
#define TASK_RUNNING            0x00000000
#define TASK_INTERRUPTIBLE      0x00000001
#define TASK_UNINTERRUPTIBLE    0x00000002
#define TASK_STOPPED            0x00000004
#define TASK_TRACED             0x00000008
#define EXIT_DEAD               0x00000010
#define EXIT_ZOMBIE             0x00000020
#define EXIT_TRACE              (EXIT_ZOMBIE | EXIT_DEAD)
#define TASK_PARKED             0x00000040
#define TASK_DEAD               0x00000080
#define TASK_WAKEKILL           0x00000100
#define TASK_WAKING             0x00000200
#define TASK_NOLOAD             0x00000400
#define TASK_NEW                0x00000800
#define TASK_RTLOCK_WAIT        0x00001000
#define TASK_FREEZABLE          0x00002000
#define TASK_FROZEN             0x00008000
#define TASK_STATE_MAX          0x00010000

// Exact offsets from pahole with BTF
const uint32_t STATE_OFFSET = 0x30;        // __state field (offset 48)
const uint32_t USAGE_OFFSET = 0x40;        // usage refcount (offset 64)
const uint32_t FLAGS_OFFSET = 0x44;        // flags field (offset 68)
const uint32_t EXIT_STATE_OFFSET = 0x6e8;  // exit_state field (offset 1768)
const uint32_t EXIT_CODE_OFFSET = 0x6ec;   // exit_code field (offset 1772)

const uint32_t PID_OFFSET = 0x750;
const uint32_t COMM_OFFSET = 0x970;
const uint32_t MM_OFFSET = 0x6d0;
const uint32_t TASKS_LIST_OFFSET = 0x7e0;
const uint32_t TASK_STRUCT_SIZE = 9088;
const uint32_t PAGE_SIZE = 4096;

bool isKernelPointer(uint64_t ptr) {
    return (ptr >> 48) == 0xFFFF;
}

std::string getStateString(uint32_t state) {
    if (state == TASK_RUNNING) return "RUNNING";
    if (state & TASK_INTERRUPTIBLE) return "INTERRUPTIBLE";
    if (state & TASK_UNINTERRUPTIBLE) return "UNINTERRUPTIBLE";
    if (state & TASK_STOPPED) return "STOPPED";
    if (state & TASK_TRACED) return "TRACED";
    if (state & EXIT_DEAD) return "EXIT_DEAD";
    if (state & EXIT_ZOMBIE) return "ZOMBIE";
    if (state & TASK_DEAD) return "DEAD";
    if (state & TASK_PARKED) return "PARKED";
    if (state & TASK_NEW) return "NEW";
    return "UNKNOWN(" + std::to_string(state) + ")";
}

int main() {
    const char* memFile = "/tmp/haywire-vm-mem";

    int fd = open(memFile, O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << memFile << std::endl;
        return 1;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    void* memBase = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to mmap" << std::endl;
        close(fd);
        return 1;
    }

    uint8_t* mem = (uint8_t*)memBase;

    const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};
    const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};

    std::vector<uint64_t> allOffsets;
    for (auto o : SLAB_OFFSETS) allOffsets.push_back(o);
    for (auto o : PAGE_STRADDLE_OFFSETS) allOffsets.push_back(o);

    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> processesByName;
    std::set<uint32_t> uniquePids;

    std::cout << "=== Checking Task States ===" << std::endl;
    std::cout << "\nLooking for duplicate process names with different states...\n" << std::endl;

    for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
        for (auto slabOffset : allOffsets) {
            uint64_t offset = pageStart + slabOffset;
            if (offset + TASK_STRUCT_SIZE > fileSize) continue;

            // Check PID
            uint32_t pid = *(uint32_t*)(mem + offset + PID_OFFSET);
            if (!pid || pid < 1 || pid > 32768) continue;

            // Check comm
            char* comm = (char*)(mem + offset + COMM_OFFSET);

            // Basic validation
            bool validComm = false;
            for (int i = 0; i < 16; i++) {
                if (comm[i] == 0 && i > 0) {
                    validComm = true;
                    break;
                }
                if (comm[i] < 0x20 || comm[i] > 0x7E) break;
            }
            if (!validComm) continue;

            std::string name(comm, strnlen(comm, 16));

            // Read state fields
            uint32_t state = *(uint32_t*)(mem + offset + STATE_OFFSET);
            uint32_t exit_state = *(uint32_t*)(mem + offset + EXIT_STATE_OFFSET);
            uint32_t flags = *(uint32_t*)(mem + offset + FLAGS_OFFSET);

            // Check tasks list pointers
            uint64_t tasksNext = *(uint64_t*)(mem + offset + TASKS_LIST_OFFSET);
            uint64_t tasksPrev = *(uint64_t*)(mem + offset + TASKS_LIST_OFFSET + 8);
            bool hasValidList = (tasksNext != 0 && tasksPrev != 0 &&
                                 isKernelPointer(tasksNext) && isKernelPointer(tasksPrev));

            // Store state with PID
            processesByName[name].push_back({pid, state});

            // Show first few examples of each state
            if (processesByName[name].size() == 1) { // First instance of this name
                if (state != TASK_RUNNING && state != TASK_INTERRUPTIBLE) {
                    std::cout << "PID " << std::setw(5) << pid
                              << " (" << std::setw(16) << name << ")"
                              << " - State: " << getStateString(state)
                              << ", Exit: 0x" << std::hex << exit_state << std::dec
                              << ", List: " << (hasValidList ? "VALID" : "INVALID")
                              << std::endl;
                }
            }
        }
    }

    std::cout << "\n=== Processes with Multiple Instances ===" << std::endl;
    for (const auto& [name, instances] : processesByName) {
        if (instances.size() > 5) {  // Show processes with many duplicates
            std::cout << std::setw(20) << std::left << name << " : "
                      << instances.size() << " instances - ";

            // Count states
            std::map<uint32_t, int> stateCounts;
            for (const auto& [pid, state] : instances) {
                stateCounts[state]++;
            }

            // Show state distribution
            bool first = true;
            for (const auto& [state, count] : stateCounts) {
                if (!first) std::cout << ", ";
                std::cout << count << " " << getStateString(state);
                first = false;
            }
            std::cout << std::endl;

            // Show some PIDs
            std::cout << "    PIDs: ";
            for (size_t i = 0; i < std::min(size_t(10), instances.size()); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << instances[i].first;
            }
            if (instances.size() > 10) std::cout << " ...";
            std::cout << std::endl;
        }
    }

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}