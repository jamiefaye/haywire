/**
 * Test tool to verify offsets by searching for known process names
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

    std::cout << "Searching for 'systemd' at any offset..." << std::endl;

    uint8_t* mem = (uint8_t*)memBase;
    int found = 0;

    // Search for "systemd" at any offset
    for (size_t i = 0; i < fileSize - 16; i++) {
        if (memcmp(mem + i, "systemd", 7) == 0 && mem[i + 7] == 0) {
            found++;
            if (found <= 10) {
                std::cout << "\nFound 'systemd' at offset 0x" << std::hex << i << std::dec << std::endl;

                // If this is at comm offset (0x970), where would PID be?
                // PID is at 0x750, so it's at i - (0x970 - 0x750) = i - 0x220
                int64_t pidOffset = (int64_t)i - 0x220;
                if (pidOffset >= 0 && pidOffset + 4 < fileSize) {
                    uint32_t pid = *(uint32_t*)(mem + pidOffset);
                    std::cout << "  If at comm offset, PID would be: " << pid;
                    if (pid == 1) {
                        std::cout << " <- INIT/SYSTEMD FOUND!";

                        // Show task_struct start
                        int64_t taskStart = (int64_t)i - 0x970;
                        std::cout << "\n  Task struct would start at: 0x" << std::hex << taskStart << std::dec;

                        // Check mm field at offset 0x6d0
                        if (taskStart >= 0 && taskStart + 0x6d0 + 8 < fileSize) {
                            uint64_t mmPtr = *(uint64_t*)(mem + taskStart + 0x6d0);
                            std::cout << "\n  MM pointer at +0x6d0: 0x" << std::hex << mmPtr << std::dec;

                            // Check if it looks like a kernel VA
                            if ((mmPtr & 0xFFFF000000000000) == 0xFFFF000000000000) {
                                std::cout << " (kernel VA)";
                            }
                        }
                    }
                    std::cout << std::endl;
                }
            }
        }
    }

    std::cout << "\nTotal 'systemd' strings found: " << found << std::endl;

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}