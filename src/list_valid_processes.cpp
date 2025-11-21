/**
 * List all unique process names that have valid kernel linked lists
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

// From web version KernelConstants - EXACT VALUES
const uint32_t PID_OFFSET = 0x750;
const uint32_t COMM_OFFSET = 0x970;
const uint32_t MM_OFFSET = 0x6d0;
const uint32_t TASKS_LIST_OFFSET = 0x7e0;  // NOT 0x7e8!
const uint32_t TASK_STRUCT_SIZE = 9088;
const uint32_t PAGE_SIZE = 4096;

bool isKernelPointer(uint64_t ptr) {
    // Kernel pointers have top 16 bits = 0xFFFF
    return (ptr >> 48) == 0xFFFF;
}

bool validateLinkedList(uint8_t* mem, uint64_t offset) {
    uint64_t listOffset = offset + TASKS_LIST_OFFSET;
    uint64_t* nextPtr = (uint64_t*)(mem + listOffset);
    uint64_t* prevPtr = (uint64_t*)(mem + listOffset + 8);

    if (*nextPtr == 0 || *prevPtr == 0) {
        return false;
    }

    if (!isKernelPointer(*nextPtr) || !isKernelPointer(*prevPtr)) {
        return false;
    }

    return true;
}

bool checkTaskStruct(uint8_t* mem, uint64_t offset, uint64_t fileSize,
                     std::string& nameOut, uint32_t& pidOut) {
    if (offset + TASK_STRUCT_SIZE > fileSize) return false;

    // Check PID
    uint32_t pid = *(uint32_t*)(mem + offset + PID_OFFSET);
    if (!pid || pid < 1 || pid > 32768) {
        return false;
    }

    // Check comm (process name)
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

    // If no null found, check printability
    if (nullIdx == -1) {
        int printable = 0;
        for (int i = 0; i < 4 && i < 16; i++) {
            if (comm[i] >= 32 && comm[i] <= 126) printable++;
        }
        if (printable < 2) return false;
        nullIdx = 16;
    }

    // Check for printable ASCII
    for (int i = 0; i < nullIdx; i++) {
        if (comm[i] < 0x20 || comm[i] > 0x7E) {
            return false;
        }
    }

    // MUST have valid linked list
    if (!validateLinkedList(mem, offset)) {
        return false;
    }

    // Output values
    pidOut = pid;
    nameOut = std::string((char*)comm, nullIdx);

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

    std::cout << "Finding all processes with valid kernel linked lists..." << std::endl;
    std::cout << std::endl;

    const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};
    const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};

    std::vector<uint64_t> allOffsets;
    for (auto o : SLAB_OFFSETS) allOffsets.push_back(o);
    for (auto o : PAGE_STRADDLE_OFFSETS) allOffsets.push_back(o);

    std::map<std::string, std::vector<uint32_t>> processesByName;
    std::set<uint32_t> uniquePids;

    for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
        for (auto slabOffset : allOffsets) {
            uint64_t offset = pageStart + slabOffset;
            std::string name;
            uint32_t pid;

            if (checkTaskStruct(mem, offset, fileSize, name, pid)) {
                processesByName[name].push_back(pid);
                uniquePids.insert(pid);
            }
        }
    }

    std::cout << "=== UNIQUE PROCESS NAMES ===" << std::endl;
    std::cout << "Found " << processesByName.size() << " unique names, "
              << uniquePids.size() << " unique PIDs" << std::endl;
    std::cout << std::endl;

    // Sort by name for readability
    std::map<std::string, std::vector<uint32_t>>::iterator it;
    for (it = processesByName.begin(); it != processesByName.end(); ++it) {
        std::cout << std::setw(20) << std::left << it->first << " : ";

        // Show first few PIDs
        std::cout << "PIDs: ";
        for (size_t i = 0; i < it->second.size() && i < 5; i++) {
            std::cout << it->second[i];
            if (i < it->second.size() - 1 && i < 4) std::cout << ", ";
        }
        if (it->second.size() > 5) {
            std::cout << " ... (" << it->second.size() << " total)";
        }

        // Check if it's a system process
        const char* sysProcs[] = {"systemd", "init", "kworker", "kthread", "ksoftirq",
                                  "migration", "rcu_", "sshd", "bash", "dbus"};
        for (auto sp : sysProcs) {
            if (it->first.find(sp) != std::string::npos) {
                std::cout << " <-- SYSTEM";
                break;
            }
        }

        std::cout << std::endl;
    }

    // Show summary of common process types
    std::cout << "\n=== PROCESS TYPE SUMMARY ===" << std::endl;

    int kworkerCount = 0, chromeCount = 0, firefoxCount = 0;
    int systemdCount = 0, kernelThreads = 0;

    for (it = processesByName.begin(); it != processesByName.end(); ++it) {
        if (it->first.find("kworker") != std::string::npos) kworkerCount++;
        if (it->first.find("Isolated Web Co") != std::string::npos) chromeCount++;
        if (it->first.find("firefox") != std::string::npos) firefoxCount++;
        if (it->first.find("systemd") != std::string::npos) systemdCount++;
        if (it->first[0] == '[' && it->first[it->first.size()-1] == ']') kernelThreads++;
    }

    std::cout << "kworker processes: " << kworkerCount << std::endl;
    std::cout << "Chrome/Isolated Web: " << chromeCount << std::endl;
    std::cout << "Firefox processes: " << firefoxCount << std::endl;
    std::cout << "systemd processes: " << systemdCount << std::endl;
    std::cout << "Kernel threads []: " << kernelThreads << std::endl;

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}