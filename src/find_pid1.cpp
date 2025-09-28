/**
 * Find PID=1 in memory and check what's at comm offset
 */

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

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

    std::cout << "Searching for PID=1 (as uint32_t)..." << std::endl;

    uint8_t* mem = (uint8_t*)memBase;
    int found = 0;

    // Search for PID=1 as a 32-bit integer
    for (size_t i = 0; i < fileSize - 4; i += 4) {  // Align to 4 bytes for PIDs
        uint32_t* pidPtr = (uint32_t*)(mem + i);
        if (*pidPtr == 1) {
            // This could be PID=1 at offset 0x750
            // Check if there's a reasonable comm at +0x220
            size_t commOffset = i + 0x220;
            if (commOffset + 16 < fileSize) {
                char* comm = (char*)(mem + commOffset);

                // Check if it looks like a valid process name
                bool hasNull = false;
                bool allPrintable = true;
                int nullIdx = -1;

                for (int j = 0; j < 16; j++) {
                    if (comm[j] == 0) {
                        hasNull = true;
                        nullIdx = j;
                        break;
                    }
                    if (comm[j] < 0x20 || comm[j] > 0x7E) {
                        allPrintable = false;
                        break;
                    }
                }

                if (hasNull && allPrintable && nullIdx > 0 && nullIdx <= 15) {
                    found++;
                    if (found <= 20) {
                        std::cout << "\nPotential PID=1 at offset 0x" << std::hex << i << std::dec;
                        std::cout << "\n  Comm at +0x220: \"" << std::string(comm, nullIdx) << "\"";

                        // Check tasks list pointers
                        int64_t taskStart = (int64_t)i - 0x750;
                        if (taskStart >= 0) {
                            std::cout << "\n  Task struct would start at: 0x" << std::hex << taskStart << std::dec;

                            // Check next/prev at 0x7e8 and 0x7f0
                            if (taskStart + 0x7f0 + 8 < fileSize) {
                                uint64_t next = *(uint64_t*)(mem + taskStart + 0x7e8);
                                uint64_t prev = *(uint64_t*)(mem + taskStart + 0x7f0);
                                std::cout << "\n  tasks.next: 0x" << std::hex << next;
                                std::cout << "\n  tasks.prev: 0x" << std::hex << prev;

                                // Check if they look like kernel VAs
                                if ((next & 0xFFFF000000000000) == 0xFFFF000000000000) {
                                    std::cout << " (kernel VA)";
                                }
                                std::cout << std::dec;
                            }
                        }
                        std::cout << std::endl;
                    }
                }
            }
        }
    }

    std::cout << "\nTotal potential PID=1 locations with valid comm: " << found << std::endl;

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}