#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// Correct offsets from pahole
const size_t TASKS_OFFSET = 0x680;  // task_struct.tasks (was wrong: 0x7e0)
const size_t MM_OFFSET = 0x6d0;     // task_struct.mm
const size_t PID_OFFSET = 0x750;    // task_struct.pid
const size_t COMM_OFFSET = 0x970;   // task_struct.comm

void* memBase = nullptr;
size_t memorySize = 0;

// Hardcode values we got from QMP
const uint64_t SWAPPER_PGD = 0x136deb000;
const uint64_t CURRENT_TASK = 0xffff80007c8e4000;

bool IsKernelPointer(uint64_t addr) {
    return (addr & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL;
}

uint64_t TranslateVA(uint64_t va, uint64_t pgdBase) {
    const uint64_t VALID_BIT = 1;
    const uint64_t TABLE_BIT = 2;
    const uint64_t PA_MASK = 0x0000FFFFFFFFF000ULL;

    uint32_t pgdIndex = (va >> 39) & 0x1FF;
    uint32_t pudIndex = (va >> 30) & 0x1FF;
    uint32_t pmdIndex = (va >> 21) & 0x1FF;
    uint32_t pteIndex = (va >> 12) & 0x1FF;
    uint32_t pageOffset = va & 0xFFF;

    std::cout << "Translating VA 0x" << std::hex << va << std::dec << "\n";
    std::cout << "  PGD index: " << pgdIndex << ", PUD: " << pudIndex
              << ", PMD: " << pmdIndex << ", PTE: " << pteIndex << "\n";

    if (pgdBase < 0x40000000 || pgdBase >= 0x40000000 + memorySize) return 0;

    uint64_t pgdOffset = (pgdBase - 0x40000000) + (pgdIndex * 8);
    if (pgdOffset + 8 > memorySize) {
        std::cout << "  PGD offset out of bounds\n";
        return 0;
    }
    uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);
    std::cout << "  PGD entry: 0x" << std::hex << pgdEntry << std::dec << "\n";
    if (!(pgdEntry & VALID_BIT)) {
        std::cout << "  PGD entry not valid\n";
        return 0;
    }
    if (!(pgdEntry & TABLE_BIT)) {
        std::cout << "  PGD entry not a table\n";
        return 0;
    }

    uint64_t pudBase = pgdEntry & PA_MASK;
    std::cout << "  PUD base PA: 0x" << std::hex << pudBase << std::dec << "\n";
    uint64_t pudOffset = (pudBase - 0x40000000) + (pudIndex * 8);
    if (pudOffset + 8 > memorySize) {
        std::cout << "  PUD offset out of bounds: 0x" << std::hex << pudOffset << std::dec << "\n";
        return 0;
    }
    uint64_t pudEntry = *(uint64_t*)((uint8_t*)memBase + pudOffset);
    std::cout << "  PUD entry: 0x" << std::hex << pudEntry << std::dec << "\n";
    std::cout << "  PUD entry VALID_BIT: " << ((pudEntry & VALID_BIT) ? "set" : "not set") << "\n";
    std::cout << "  PUD entry TABLE_BIT: " << ((pudEntry & TABLE_BIT) ? "set" : "not set") << "\n";
    if (!(pudEntry & VALID_BIT)) {
        std::cout << "  PUD entry not valid\n";
        return 0;
    }

    if (!(pudEntry & TABLE_BIT)) {
        // 1GB huge page
        uint64_t pa = (pudEntry & 0x0000FFFFC0000000ULL) | (va & 0x3FFFFFFF);
        std::cout << "  PUD is 1GB huge page, PA: 0x" << std::hex << pa << std::dec << "\n";
        return pa;
    }

    uint64_t pmdBase = pudEntry & PA_MASK;
    std::cout << "  PMD base PA: 0x" << std::hex << pmdBase << std::dec << "\n";

    // Convert PA to offset in memory-backend-file
    // The file starts at PA 0x40000000
    if (pmdBase < 0x40000000 || pmdBase >= 0x40000000 + memorySize) {
        std::cout << "  PMD base PA outside memory range (0x40000000 - 0x"
                  << std::hex << (0x40000000 + memorySize) << ")\n";
        return 0;
    }

    uint64_t pmdOffset = (pmdBase - 0x40000000) + (pmdIndex * 8);
    std::cout << "  PMD offset in file: 0x" << std::hex << pmdOffset << std::dec << "\n";

    if (pmdOffset + 8 > memorySize) {
        std::cout << "  PMD offset out of bounds: 0x" << std::hex << pmdOffset << std::dec << "\n";
        return 0;
    }
    uint64_t pmdEntry = *(uint64_t*)((uint8_t*)memBase + pmdOffset);
    std::cout << "  PMD entry: 0x" << std::hex << pmdEntry << std::dec << "\n";
    if (!(pmdEntry & VALID_BIT)) {
        std::cout << "  PMD entry not valid\n";
        return 0;
    }

    if (!(pmdEntry & TABLE_BIT)) {
        return (pmdEntry & 0x0000FFFFFFE00000ULL) | (va & 0x1FFFFF);
    }

    uint64_t pteBase = pmdEntry & PA_MASK;
    uint64_t pteOffset = (pteBase - 0x40000000) + (pteIndex * 8);
    if (pteOffset + 8 > memorySize) return 0;
    uint64_t pteEntry = *(uint64_t*)((uint8_t*)memBase + pteOffset);
    if (!(pteEntry & VALID_BIT)) return 0;

    return (pteEntry & PA_MASK) | pageOffset;
}

int main() {
    // Open memory file
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

    std::cout << "Memory mapped: " << (memorySize / 1024 / 1024) << " MB\n";
    std::cout << "Using hardcoded values from QMP:\n";
    std::cout << "  Swapper PGD: 0x" << std::hex << SWAPPER_PGD << std::dec << "\n";
    std::cout << "  Current task: 0x" << std::hex << CURRENT_TASK << std::dec << "\n\n";

    // Translate current_task to PA
    uint64_t currentTaskPA = TranslateVA(CURRENT_TASK, SWAPPER_PGD);
    std::cout << "Current task PA: 0x" << std::hex << currentTaskPA << std::dec << "\n";

    if (currentTaskPA < 0x40000000 || currentTaskPA >= 0x40000000 + memorySize) {
        std::cerr << "Failed to translate current_task\n";
        return 1;
    }

    // Examine current task first
    uint64_t offset = currentTaskPA - 0x40000000;
    uint8_t* task = (uint8_t*)memBase + offset;

    std::cout << "\n=== Current Task Examination ===\n";
    uint32_t pid = *(uint32_t*)(task + PID_OFFSET);
    char* comm = (char*)(task + COMM_OFFSET);
    uint64_t mm = *(uint64_t*)(task + MM_OFFSET);
    uint64_t tasksNext = *(uint64_t*)(task + TASKS_OFFSET);
    uint64_t tasksPrev = *(uint64_t*)(task + TASKS_OFFSET + 8);

    std::cout << "PID: " << pid << "\n";
    std::cout << "Comm: '" << std::string(comm, strnlen(comm, 16)) << "'\n";
    std::cout << "MM: 0x" << std::hex << mm << std::dec << "\n";
    std::cout << "tasks.next: 0x" << std::hex << tasksNext << std::dec << "\n";
    std::cout << "tasks.prev: 0x" << std::hex << tasksPrev << std::dec << "\n";

    // Now walk the list
    std::cout << "\n=== Walking Process List ===\n";
    std::cout << "Using CORRECT offset 0x680 (not 0x7e0):\n\n";

    std::set<uint64_t> visited;
    uint64_t currentPA = currentTaskPA;
    int count = 0;
    const int MAX_PROCESSES = 200;

    while (count < MAX_PROCESSES) {
        if (visited.count(currentPA)) {
            std::cout << "\nReached already-visited task (circular list complete)\n";
            break;
        }
        visited.insert(currentPA);

        offset = currentPA - 0x40000000;
        if (offset >= memorySize) {
            std::cerr << "Task offset out of bounds\n";
            break;
        }

        task = (uint8_t*)memBase + offset;

        // Read with correct offsets
        pid = *(uint32_t*)(task + PID_OFFSET);
        comm = (char*)(task + COMM_OFFSET);
        mm = *(uint64_t*)(task + MM_OFFSET);
        tasksNext = *(uint64_t*)(task + TASKS_OFFSET);

        if (pid > 0 && pid < 100000) {
            std::string name(comm, strnlen(comm, 16));
            std::cout << "PID " << std::setw(6) << pid << ": " << std::setw(16) << name;
            if (mm == 0) {
                std::cout << " [kernel]";
            }
            std::cout << "\n";
        }

        // Follow tasks.next
        if (!IsKernelPointer(tasksNext)) {
            std::cerr << "\ntasks.next not kernel pointer: 0x" << std::hex << tasksNext << std::dec << "\n";
            std::cerr << "This means offset 0x680 is reading wrong data\n";
            break;
        }

        // tasks.next points to tasks field of next task, subtract offset
        uint64_t nextTaskVA = tasksNext - TASKS_OFFSET;
        uint64_t nextTaskPA = TranslateVA(nextTaskVA, SWAPPER_PGD);

        if (nextTaskPA == 0) {
            std::cerr << "\nFailed to translate next task VA: 0x" << std::hex << nextTaskVA << std::dec << "\n";
            break;
        }

        currentPA = nextTaskPA;
        count++;
    }

    std::cout << "\nProcessed " << count << " tasks, visited " << visited.size() << " unique\n";

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}