#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Correct offsets from pahole
const size_t TASKS_OFFSET = 0x680;  // task_struct.tasks
const size_t MM_OFFSET = 0x6d0;     // task_struct.mm
const size_t PID_OFFSET = 0x750;    // task_struct.pid
const size_t COMM_OFFSET = 0x970;   // task_struct.comm

void* memBase = nullptr;
size_t memorySize = 0;

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

    // Read PGD entry
    if (pgdBase < 0x40000000 || pgdBase >= 0x40000000 + memorySize) return 0;

    uint64_t pgdOffset = (pgdBase - 0x40000000) + (pgdIndex * 8);
    if (pgdOffset + 8 > memorySize) return 0;
    uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);
    if (!(pgdEntry & VALID_BIT)) return 0;
    if (!(pgdEntry & TABLE_BIT)) return 0;

    // Read PUD entry
    uint64_t pudBase = pgdEntry & PA_MASK;
    uint64_t pudOffset = (pudBase - 0x40000000) + (pudIndex * 8);
    if (pudOffset + 8 > memorySize) return 0;
    uint64_t pudEntry = *(uint64_t*)((uint8_t*)memBase + pudOffset);
    if (!(pudEntry & VALID_BIT)) return 0;

    // Check for 1GB huge page
    if (!(pudEntry & TABLE_BIT)) {
        return (pudEntry & 0x0000FFFFC0000000ULL) | (va & 0x3FFFFFFF);
    }

    // Read PMD entry
    uint64_t pmdBase = pudEntry & PA_MASK;
    uint64_t pmdOffset = (pmdBase - 0x40000000) + (pmdIndex * 8);
    if (pmdOffset + 8 > memorySize) return 0;
    uint64_t pmdEntry = *(uint64_t*)((uint8_t*)memBase + pmdOffset);
    if (!(pmdEntry & VALID_BIT)) return 0;

    // Check for 2MB huge page
    if (!(pmdEntry & TABLE_BIT)) {
        return (pmdEntry & 0x0000FFFFFFE00000ULL) | (va & 0x1FFFFF);
    }

    // Read PTE entry
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

    std::cout << "Memory mapped: " << (memorySize / 1024 / 1024) << " MB\n\n";

    // Get kernel info from QMP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4445);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to QMP\n";
        return 1;
    }

    char buffer[8192];
    recv(sock, buffer, sizeof(buffer), 0);
    send(sock, "{\"execute\": \"qmp_capabilities\"}\n", 33, 0);
    recv(sock, buffer, sizeof(buffer), 0);

    std::string cmd = "{\"execute\": \"query-kernel-info\", \"arguments\": {\"cpu-index\": 0}}\n";
    send(sock, cmd.c_str(), cmd.length(), 0);
    int len = recv(sock, buffer, sizeof(buffer)-1, 0);
    buffer[len] = '\0';
    close(sock);

    std::string response(buffer);
    std::cout << "QMP Response: " << response.substr(0, 200) << "...\n\n";

    // Parse swapper PGD
    uint64_t swapperPGD = 0;
    size_t pos = response.find("\"ttbr1\":");
    if (pos != std::string::npos) {
        size_t start = pos + 8;
        while (response[start] == ' ' || response[start] == ':') start++;
        size_t end = start;
        while (isdigit(response[end]) || (response[end] >= 'a' && response[end] <= 'f')) end++;
        swapperPGD = std::stoull(response.substr(start, end-start), nullptr, 0) & 0xFFFFFFFFF000ULL;
    }

    // Parse current task
    uint64_t currentTask = 0;
    pos = response.find("\"current-task\":");
    if (pos != std::string::npos) {
        size_t start = pos + 16;
        while (response[start] == ' ' || response[start] == ':') start++;
        size_t end = start;
        while (isdigit(response[end]) || (response[end] >= 'a' && response[end] <= 'f')) end++;
        currentTask = std::stoull(response.substr(start, end-start), nullptr, 0);
    }

    std::cout << "Swapper PGD: 0x" << std::hex << swapperPGD << std::dec << "\n";
    std::cout << "Current task VA: 0x" << std::hex << currentTask << std::dec << "\n";

    if (!IsKernelPointer(currentTask)) {
        std::cerr << "current_task doesn't look like kernel VA\n";
        return 1;
    }

    // Translate current_task to PA
    uint64_t currentTaskPA = TranslateVA(currentTask, swapperPGD);
    std::cout << "Current task PA: 0x" << std::hex << currentTaskPA << std::dec << "\n\n";

    if (currentTaskPA < 0x40000000 || currentTaskPA >= 0x40000000 + memorySize) {
        std::cerr << "Failed to translate current_task\n";
        return 1;
    }

    // Now walk the process list using CORRECT offsets
    std::cout << "=== Walking Process List (with correct offsets) ===\n";

    std::set<uint64_t> visited;
    uint64_t startPA = currentTaskPA;
    uint64_t currentPA = startPA;
    int count = 0;
    const int MAX_PROCESSES = 500;

    do {
        if (count++ > MAX_PROCESSES) {
            std::cerr << "Too many processes, stopping\n";
            break;
        }

        if (visited.count(currentPA)) {
            std::cout << "\nCompleted circular list walk\n";
            break;
        }
        visited.insert(currentPA);

        // Read task_struct
        uint64_t offset = currentPA - 0x40000000;
        if (offset >= memorySize) {
            std::cerr << "Task offset out of bounds\n";
            break;
        }

        uint8_t* task = (uint8_t*)memBase + offset;

        // Read fields with CORRECT offsets
        uint32_t pid = *(uint32_t*)(task + PID_OFFSET);
        char* comm = (char*)(task + COMM_OFFSET);
        uint64_t mm = *(uint64_t*)(task + MM_OFFSET);
        uint64_t tasksNext = *(uint64_t*)(task + TASKS_OFFSET);      // 0x680 not 0x7e0!
        uint64_t tasksPrev = *(uint64_t*)(task + TASKS_OFFSET + 8);  // 0x688 not 0x7e8!

        // Validate and print
        if (pid > 0 && pid < 100000) {
            std::string name(comm, strnlen(comm, 16));
            std::cout << "PID " << std::setw(6) << pid << ": " << std::setw(16) << name;
            if (mm == 0) {
                std::cout << " [kernel thread]";
            }
            std::cout << "\n";
        }

        // Follow tasks.next to next process
        if (!IsKernelPointer(tasksNext)) {
            std::cerr << "tasks.next not a kernel pointer: 0x" << std::hex << tasksNext << std::dec << "\n";
            break;
        }

        // tasks.next points to the tasks field of next task_struct
        // Subtract TASKS_OFFSET to get actual task_struct address
        uint64_t nextTaskVA = tasksNext - TASKS_OFFSET;
        uint64_t nextTaskPA = TranslateVA(nextTaskVA, swapperPGD);

        if (nextTaskPA == 0) {
            std::cerr << "Failed to translate next task\n";
            break;
        }

        currentPA = nextTaskPA;

        // Check if we're back at start
        if (currentPA == startPA) {
            std::cout << "\nCompleted circular list\n";
            break;
        }

    } while (true);

    std::cout << "\nFound " << visited.size() << " processes\n";

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}