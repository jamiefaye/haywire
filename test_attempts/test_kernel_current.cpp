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

// Copy from kernel_discovery.cpp
struct KernelInfo {
    uint64_t swapper_pgd;
    uint64_t current_task;
} kernelInfo;

void* memBase = nullptr;
size_t memorySize = 0;

bool IsKernelPointer(uint64_t addr) {
    return (addr & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL;
}

bool IsPrintableString(const std::string& str) {
    if (str.empty()) return false;
    for (char c : str) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
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

    std::cout << "Translating VA 0x" << std::hex << va << std::dec << ":\n";
    std::cout << "  PGD index: " << pgdIndex << ", PUD index: " << pudIndex
              << ", PMD index: " << pmdIndex << ", PTE index: " << pteIndex << "\n";

    // Read PGD entry
    if (pgdBase < 0x40000000 || pgdBase >= 0x40000000 + memorySize) {
        std::cerr << "PGD base out of range\n";
        return 0;
    }

    uint64_t pgdOffset = (pgdBase - 0x40000000) + (pgdIndex * 8);
    if (pgdOffset + 8 > memorySize) return 0;
    uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);

    std::cout << "  PGD entry: 0x" << std::hex << pgdEntry << std::dec;
    if (!(pgdEntry & VALID_BIT)) {
        std::cout << " (invalid)\n";
        return 0;
    }
    if (!(pgdEntry & TABLE_BIT)) {
        std::cout << " (not a table)\n";
        return 0;
    }
    std::cout << " (valid table)\n";

    // Read PUD entry
    uint64_t pudBase = pgdEntry & PA_MASK;
    uint64_t pudOffset = (pudBase - 0x40000000) + (pudIndex * 8);
    if (pudOffset + 8 > memorySize) return 0;
    uint64_t pudEntry = *(uint64_t*)((uint8_t*)memBase + pudOffset);

    std::cout << "  PUD entry: 0x" << std::hex << pudEntry << std::dec;
    if (!(pudEntry & VALID_BIT)) {
        std::cout << " (invalid)\n";
        return 0;
    }

    // Check if PUD is huge page (1GB)
    if (!(pudEntry & TABLE_BIT)) {
        uint64_t pa = (pudEntry & 0x0000FFFFC0000000ULL) | (va & 0x3FFFFFFF);
        std::cout << " (1GB huge page) -> PA 0x" << std::hex << pa << std::dec << "\n";
        return pa;
    }
    std::cout << " (valid table)\n";

    // Read PMD entry
    uint64_t pmdBase = pudEntry & PA_MASK;
    uint64_t pmdOffset = (pmdBase - 0x40000000) + (pmdIndex * 8);
    if (pmdOffset + 8 > memorySize) return 0;
    uint64_t pmdEntry = *(uint64_t*)((uint8_t*)memBase + pmdOffset);

    std::cout << "  PMD entry: 0x" << std::hex << pmdEntry << std::dec;
    if (!(pmdEntry & VALID_BIT)) {
        std::cout << " (invalid)\n";
        return 0;
    }

    // Check if PMD is huge page (2MB)
    if (!(pmdEntry & TABLE_BIT)) {
        uint64_t pa = (pmdEntry & 0x0000FFFFFFE00000ULL) | (va & 0x1FFFFF);
        std::cout << " (2MB huge page) -> PA 0x" << std::hex << pa << std::dec << "\n";
        return pa;
    }
    std::cout << " (valid table)\n";

    // Read PTE entry
    uint64_t pteBase = pmdEntry & PA_MASK;
    uint64_t pteOffset = (pteBase - 0x40000000) + (pteIndex * 8);
    if (pteOffset + 8 > memorySize) return 0;
    uint64_t pteEntry = *(uint64_t*)((uint8_t*)memBase + pteOffset);

    std::cout << "  PTE entry: 0x" << std::hex << pteEntry << std::dec;
    if (!(pteEntry & VALID_BIT)) {
        std::cout << " (invalid)\n";
        return 0;
    }

    // 4KB page
    uint64_t pa = (pteEntry & PA_MASK) | pageOffset;
    std::cout << " -> PA 0x" << std::hex << pa << std::dec << "\n";
    return pa;
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

    // Connect to QMP and get kernel info
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

    // Read greeting and send capabilities
    char buffer[8192];
    recv(sock, buffer, sizeof(buffer), 0);
    send(sock, "{\"execute\": \"qmp_capabilities\"}\n", 33, 0);
    recv(sock, buffer, sizeof(buffer), 0);

    // Query kernel info
    const char* cmd = "{\"execute\": \"query-kernel-info\", \"arguments\": {\"cpu-index\": 0}}\n";
    std::cout << "Sending command: " << cmd;
    send(sock, cmd, strlen(cmd), 0);
    int len = recv(sock, buffer, sizeof(buffer)-1, 0);
    buffer[len] = '\0';

    std::string response(buffer);
    std::cout << "QMP Response:\n" << response << "\n\n";

    // Parse swapper PGD
    size_t pos = response.find("\"ttbr1\":");
    if (pos != std::string::npos) {
        size_t start = pos + 8;
        while (response[start] == ' ') start++;
        size_t end = start;
        while (isdigit(response[end]) || response[end] == 'x' ||
               (response[end] >= 'a' && response[end] <= 'f') ||
               (response[end] >= 'A' && response[end] <= 'F')) end++;
        kernelInfo.swapper_pgd = std::stoull(response.substr(start, end-start), nullptr, 0) & 0xFFFFFFFFF000ULL;
        std::cout << "Swapper PGD: 0x" << std::hex << kernelInfo.swapper_pgd << std::dec << "\n";
    }

    // Parse current task
    pos = response.find("\"current-task\":");
    if (pos != std::string::npos) {
        size_t start = pos + 16;
        while (response[start] == ' ') start++;
        size_t end = start;
        while (isdigit(response[end]) || response[end] == 'x' ||
               (response[end] >= 'a' && response[end] <= 'f') ||
               (response[end] >= 'A' && response[end] <= 'F')) end++;
        kernelInfo.current_task = std::stoull(response.substr(start, end-start), nullptr, 0);
        std::cout << "Current task: 0x" << std::hex << kernelInfo.current_task << std::dec << "\n\n";
    }

    close(sock);

    // Now examine current_task
    if (IsKernelPointer(kernelInfo.current_task)) {
        std::cout << "current_task is a kernel VA, translating...\n";

        uint64_t taskPA = TranslateVA(kernelInfo.current_task, kernelInfo.swapper_pgd);

        if (taskPA && taskPA >= 0x40000000 && taskPA < 0x40000000 + memorySize) {
            std::cout << "\nExamining task_struct at PA 0x" << std::hex << taskPA << std::dec << ":\n";

            uint64_t offset = taskPA - 0x40000000;
            uint8_t* task = (uint8_t*)memBase + offset;

            // Check PID
            uint32_t pid = *(uint32_t*)(task + 0x750);
            std::cout << "  PID (0x750): " << pid << "\n";

            // Check comm
            char* comm = (char*)(task + 0x970);
            std::string name(comm, strnlen(comm, 16));
            std::cout << "  Comm (0x970): '" << name << "'\n";

            // Check mm
            uint64_t mm = *(uint64_t*)(task + 0x930);
            std::cout << "  mm_struct (0x930): 0x" << std::hex << mm << std::dec << "\n";

            // Check tasks list
            uint64_t next = *(uint64_t*)(task + 0x7e0);
            uint64_t prev = *(uint64_t*)(task + 0x7e8);
            std::cout << "  tasks.next (0x7e0): 0x" << std::hex << next << std::dec << "\n";
            std::cout << "  tasks.prev (0x7e8): 0x" << std::hex << prev << std::dec << "\n";

            if (pid > 0 && pid < 100000 && IsPrintableString(name)) {
                std::cout << "\n✓ This looks like a valid task_struct!\n";
                std::cout << "Process: PID " << pid << " (" << name << ")\n";
            } else {
                std::cout << "\n✗ Doesn't look valid\n";
            }
        } else {
            std::cout << "Translation failed or PA out of range\n";
        }
    }

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}