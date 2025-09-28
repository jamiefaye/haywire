/**
 * Count kernel threads (mm == 0) vs user processes
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

const uint32_t PID_OFFSET = 0x750;
const uint32_t COMM_OFFSET = 0x970;
const uint32_t MM_OFFSET = 0x6d0;
const uint32_t TASKS_LIST_OFFSET = 0x7e0;
const uint32_t TASK_STRUCT_SIZE = 9088;
const uint32_t PAGE_SIZE = 4096;

bool isKernelPointer(uint64_t ptr) {
    return (ptr >> 48) == 0xFFFF;
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

    int kernelThreads = 0;
    int userProcesses = 0;
    std::set<uint32_t> uniquePids;

    for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
        for (auto slabOffset : allOffsets) {
            uint64_t offset = pageStart + slabOffset;
            if (offset + TASK_STRUCT_SIZE > fileSize) continue;

            // Check PID
            uint32_t pid = *(uint32_t*)(mem + offset + PID_OFFSET);
            if (!pid || pid < 1 || pid > 32768) continue;

            // Skip duplicates
            if (uniquePids.find(pid) != uniquePids.end()) continue;

            // Check comm (basic validation)
            uint8_t* comm = mem + offset + COMM_OFFSET;
            bool validComm = false;
            for (int i = 0; i < 16; i++) {
                if (comm[i] == 0 && i > 0) {
                    validComm = true;
                    break;
                }
                if (comm[i] < 0x20 || comm[i] > 0x7E) break;
            }
            if (!validComm) continue;

            // Check mm pointer
            uint64_t mmPtr = *(uint64_t*)(mem + offset + MM_OFFSET);

            // Count as valid process
            uniquePids.insert(pid);
            std::string name((char*)comm, strnlen((char*)comm, 16));

            if (mmPtr == 0) {
                kernelThreads++;
                std::cout << "KERNEL: PID " << std::setw(5) << pid << " - " << name << std::endl;
            } else {
                userProcesses++;
                if (userProcesses <= 20) {  // Show first 20 user processes
                    std::cout << "USER:   PID " << std::setw(5) << pid << " - " << name
                              << " (mm=0x" << std::hex << mmPtr << std::dec << ")" << std::endl;
                }
            }
        }
    }

    std::cout << "\n=== SUMMARY ===" << std::endl;
    std::cout << "Kernel threads (mm==0): " << kernelThreads << std::endl;
    std::cout << "User processes (mm!=0): " << userProcesses << std::endl;
    std::cout << "Total unique PIDs: " << uniquePids.size() << std::endl;

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}