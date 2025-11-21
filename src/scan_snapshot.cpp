/**
 * Scan the snapshot for task_structs using exact web offsets
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

// From web version KernelConstants
const uint32_t PID_OFFSET = 0x750;
const uint32_t COMM_OFFSET = 0x970;
const uint32_t MM_OFFSET = 0x6d0;
const uint32_t TASKS_NEXT_OFFSET = 0x7e8;
const uint32_t TASKS_PREV_OFFSET = 0x7f0;
const uint32_t TASK_STRUCT_SIZE = 9088;
const uint32_t PAGE_SIZE = 4096;

bool checkTaskStruct(uint8_t* mem, uint64_t offset, uint64_t fileSize) {
    if (offset + TASK_STRUCT_SIZE > fileSize) return false;

    // Check PID - exactly like web version
    uint32_t pid = *(uint32_t*)(mem + offset + PID_OFFSET);
    if (!pid || pid < 1 || pid > 32768) {
        return false;
    }

    // Check comm (process name) - readString logic from web
    uint8_t* comm = mem + offset + COMM_OFFSET;

    // Find null terminator
    int nullIdx = -1;
    for (int i = 0; i < 16; i++) {
        if (comm[i] == 0) {
            nullIdx = i;
            break;
        }
    }

    // Web version: if (nullIdx === 0 || nullIdx > 15)
    if (nullIdx == 0 || nullIdx > 15) {
        return false;
    }

    // If no null found, nullIdx is -1, which is NOT > 15, so we continue
    // But we need at least some printable chars
    if (nullIdx == -1) {
        // Check if first few chars are printable (from paged-memory.ts)
        int printable = 0;
        for (int i = 0; i < std::min(4, 16); i++) {
            if (comm[i] >= 32 && comm[i] <= 126) printable++;
        }
        if (printable < 2) return false;
        nullIdx = 16; // Use full buffer
    }

    // Check for printable ASCII - web uses regex /^[\x20-\x7E]+$/
    for (int i = 0; i < nullIdx; i++) {
        if (comm[i] < 0x20 || comm[i] > 0x7E) {
            return false;
        }
    }

    return true;
}

int main() {
    const char* memFile = "haywire-vm-mem";

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

    std::cout << "Scanning snapshot: " << memFile << std::endl;
    std::cout << "File size: " << fileSize / (1024*1024) << " MB" << std::endl;
    std::cout << std::endl;

    // Exactly replicate web's offset arrays
    const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};
    const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};

    std::vector<uint64_t> allOffsets;
    for (auto o : SLAB_OFFSETS) allOffsets.push_back(o);
    for (auto o : PAGE_STRADDLE_OFFSETS) allOffsets.push_back(o);

    int foundCount = 0;
    int displayCount = 0;

    // Scan memory exactly like web version
    for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
        // Progress report every 1GB
        if (pageStart % (1024 * 1024 * 1024) == 0) {
            std::cout << "  Scanning " << pageStart / (1024 * 1024)
                      << "MB... (" << foundCount << " processes)\r" << std::flush;
        }

        for (auto slabOffset : allOffsets) {
            uint64_t offset = pageStart + slabOffset;

            if (checkTaskStruct(mem, offset, fileSize)) {
                foundCount++;

                // Display first 20 processes
                if (displayCount < 20) {
                    displayCount++;

                    uint32_t pid = *(uint32_t*)(mem + offset + PID_OFFSET);
                    char* comm = (char*)(mem + offset + COMM_OFFSET);

                    // Find actual string length
                    int len = 0;
                    for (int i = 0; i < 16; i++) {
                        if (comm[i] == 0) {
                            len = i;
                            break;
                        }
                    }
                    if (len == 0) len = 16;

                    std::string name(comm, len);

                    std::cout << "\nFound #" << displayCount
                              << " at offset 0x" << std::hex << offset << std::dec
                              << " (page 0x" << std::hex << pageStart
                              << " + " << slabOffset << std::dec << "):" << std::endl;
                    std::cout << "  PID: " << pid << std::endl;
                    std::cout << "  Name: '" << name << "'" << std::endl;

                    // Show raw bytes of comm field
                    std::cout << "  Comm bytes: ";
                    for (int i = 0; i < 16; i++) {
                        std::cout << std::hex << std::setfill('0') << std::setw(2)
                                  << (int)(uint8_t)comm[i] << " ";
                    }
                    std::cout << std::dec << std::endl;

                    // Check if it's a recognizable process name
                    if (name == "systemd" || name == "sshd" || name == "bash" ||
                        name.find("kworker") != std::string::npos ||
                        name == "init") {
                        std::cout << "  *** RECOGNIZED PROCESS! ***" << std::endl;
                    }
                }
            }
        }
    }

    std::cout << "\n\nTotal processes found: " << foundCount << std::endl;

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}