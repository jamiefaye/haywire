#include <iostream>
#include <iomanip>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

int main() {
    // Connect to QMP
    int qmpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (qmpSocket < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4445);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(qmpSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to QMP on port 4445\n";
        return 1;
    }

    // Read greeting
    char buffer[4096];
    recv(qmpSocket, buffer, sizeof(buffer), 0);
    std::cout << "QMP Greeting received\n";

    // Send capabilities
    const char* caps = "{\"execute\": \"qmp_capabilities\"}\n";
    send(qmpSocket, caps, strlen(caps), 0);
    recv(qmpSocket, buffer, sizeof(buffer), 0);

    // Query kernel info
    const char* query = "{\"execute\": \"query-kernel-info\", \"arguments\": {\"cpu-index\": 0}}\n";
    send(qmpSocket, query, strlen(query), 0);

    int received = recv(qmpSocket, buffer, sizeof(buffer)-1, 0);
    buffer[received] = '\0';

    std::cout << "QMP Response:\n" << buffer << "\n\n";

    // Parse current-task
    std::string response(buffer);
    size_t taskPos = response.find("\"current-task\":");
    if (taskPos != std::string::npos) {
        size_t start = taskPos + 16;
        while (start < response.length() && (response[start] == ' ' || response[start] == '\t')) start++;

        size_t end = start;
        while (end < response.length() && (isdigit(response[end]) || response[end] == 'x' ||
               (response[end] >= 'a' && response[end] <= 'f') ||
               (response[end] >= 'A' && response[end] <= 'F'))) {
            end++;
        }

        if (end > start) {
            std::string numStr = response.substr(start, end - start);
            uint64_t currentTask = std::stoull(numStr, nullptr, 0);

            std::cout << "current_task = 0x" << std::hex << currentTask << std::dec << "\n\n";

            // Also parse ttbr1 (swapper PGD) for translation
            uint64_t swapperPGD = 0;
            size_t ttbr1Pos = response.find("\"ttbr1\":");
            if (ttbr1Pos != std::string::npos) {
                size_t start = ttbr1Pos + 8;
                while (start < response.length() && (response[start] == ' ' || response[start] == '\t')) start++;
                size_t end = start;
                while (end < response.length() && (isdigit(response[end]) || response[end] == 'x' ||
                       (response[end] >= 'a' && response[end] <= 'f') ||
                       (response[end] >= 'A' && response[end] <= 'F'))) {
                    end++;
                }
                if (end > start) {
                    std::string numStr = response.substr(start, end - start);
                    swapperPGD = std::stoull(numStr, nullptr, 0) & 0xFFFFFFFFF000ULL;
                    std::cout << "swapper PGD (ttbr1) = 0x" << std::hex << swapperPGD << std::dec << "\n\n";
                }
            }

            // Now let's examine what's at that address
            // Open memory file
            int fd = open("/tmp/haywire-vm-mem", O_RDONLY);
            if (fd < 0) {
                std::cerr << "Failed to open memory file\n";
                close(qmpSocket);
                return 1;
            }

            struct stat st;
            fstat(fd, &st);
            size_t memorySize = st.st_size;

            void* memBase = mmap(nullptr, memorySize, PROT_READ, MAP_PRIVATE, fd, 0);
            if (memBase == MAP_FAILED) {
                std::cerr << "Failed to mmap memory\n";
                close(fd);
                close(qmpSocket);
                return 1;
            }

            // Translate kernel VA to PA using swapper PGD
            uint64_t taskPA = 0;
            if ((currentTask & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL) {
                std::cout << "current_task is a kernel virtual address, translating...\n";

                // ARM64 page table walk
                uint32_t pgdIndex = (currentTask >> 39) & 0x1FF;
                uint32_t pudIndex = (currentTask >> 30) & 0x1FF;
                uint32_t pmdIndex = (currentTask >> 21) & 0x1FF;
                uint32_t pteIndex = (currentTask >> 12) & 0x1FF;
                uint32_t pageOffset = currentTask & 0xFFF;

                const uint64_t VALID_BIT = 1;
                const uint64_t TABLE_BIT = 2;
                const uint64_t PA_MASK = 0x0000FFFFFFFFF000ULL;

                // Read PGD entry
                std::cout << "PGD index: " << pgdIndex << " (VA bits [47:39])\n";

                if (swapperPGD >= 0x40000000 && swapperPGD < 0x40000000 + memorySize) {
                    uint64_t pgdOffset = (swapperPGD - 0x40000000) + (pgdIndex * 8);
                    uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);

                    std::cout << "PGD entry at offset 0x" << std::hex << pgdOffset
                              << ": 0x" << pgdEntry << std::dec << "\n";

                    if (pgdEntry & VALID_BIT) {
                        // Read PUD entry
                        uint64_t pudBase = pgdEntry & PA_MASK;
                        uint64_t pudOffset = (pudBase - 0x40000000) + (pudIndex * 8);
                        uint64_t pudEntry = *(uint64_t*)((uint8_t*)memBase + pudOffset);

                        if (pudEntry & VALID_BIT) {
                            // Check if PUD is huge page or has PMD table
                            if (!(pudEntry & TABLE_BIT)) {
                                // 1GB huge page
                                taskPA = (pudEntry & 0x0000FFFFC0000000ULL) | (currentTask & 0x3FFFFFFF);
                            } else {
                                // Read PMD entry
                                uint64_t pmdBase = pudEntry & PA_MASK;
                                uint64_t pmdOffset = (pmdBase - 0x40000000) + (pmdIndex * 8);
                                uint64_t pmdEntry = *(uint64_t*)((uint8_t*)memBase + pmdOffset);

                                if (pmdEntry & VALID_BIT) {
                                    if (!(pmdEntry & TABLE_BIT)) {
                                        // 2MB huge page
                                        taskPA = (pmdEntry & 0x0000FFFFFFE00000ULL) | (currentTask & 0x1FFFFF);
                                    } else {
                                        // Read PTE entry
                                        uint64_t pteBase = pmdEntry & PA_MASK;
                                        uint64_t pteOffset = (pteBase - 0x40000000) + (pteIndex * 8);
                                        uint64_t pteEntry = *(uint64_t*)((uint8_t*)memBase + pteOffset);

                                        if (pteEntry & VALID_BIT) {
                                            // 4KB page
                                            taskPA = (pteEntry & PA_MASK) | pageOffset;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                std::cout << "Translated to PA: 0x" << std::hex << taskPA << std::dec << "\n\n";
            }

            // Check if we have a valid physical address to examine
            if (taskPA >= 0x40000000 && taskPA < 0x40000000 + memorySize) {
                std::cout << "Examining task_struct at PA 0x" << std::hex << taskPA << std::dec << ":\n";

                uint64_t offset = taskPA - 0x40000000;
                uint8_t* task = (uint8_t*)memBase + offset;

                // Check various offsets for task_struct fields

                // Try PID at different offsets
                std::cout << "\nPotential PIDs at various offsets:\n";
                uint32_t pid750 = *(uint32_t*)(task + 0x750);
                uint32_t pid748 = *(uint32_t*)(task + 0x748);
                uint32_t pid760 = *(uint32_t*)(task + 0x760);

                std::cout << "  0x748: " << pid748 << "\n";
                std::cout << "  0x750: " << pid750 << "\n";
                std::cout << "  0x760: " << pid760 << "\n";

                // Try comm at different offsets
                std::cout << "\nPotential comm strings:\n";
                char* comm970 = (char*)(task + 0x970);
                char* comm968 = (char*)(task + 0x968);
                char* comm980 = (char*)(task + 0x980);

                std::cout << "  0x968: '" << std::string(comm968, strnlen(comm968, 16)) << "'\n";
                std::cout << "  0x970: '" << std::string(comm970, strnlen(comm970, 16)) << "'\n";
                std::cout << "  0x980: '" << std::string(comm980, strnlen(comm980, 16)) << "'\n";

                // Check task list pointers
                std::cout << "\nTask list pointers:\n";
                uint64_t next7e0 = *(uint64_t*)(task + 0x7e0);
                uint64_t prev7e8 = *(uint64_t*)(task + 0x7e8);
                std::cout << "  tasks.next (0x7e0): 0x" << std::hex << next7e0 << std::dec << "\n";
                std::cout << "  tasks.prev (0x7e8): 0x" << std::hex << prev7e8 << std::dec << "\n";

                // Check mm pointer
                uint64_t mm930 = *(uint64_t*)(task + 0x930);
                std::cout << "\nmm_struct pointer (0x930): 0x" << std::hex << mm930 << std::dec << "\n";
            } else {
                std::cout << "Failed to translate current_task or invalid PA\n";
            }

            munmap(memBase, memorySize);
            close(fd);
        }
    }

    close(qmpSocket);
    return 0;
}