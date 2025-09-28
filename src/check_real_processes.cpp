/**
 * Check if we find any REAL processes with kernel pointer validation
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
#include <set>

// From web version KernelConstants - EXACT VALUES
const uint32_t PID_OFFSET = 0x750;
const uint32_t COMM_OFFSET = 0x970;
const uint32_t MM_OFFSET = 0x6d0;
const uint32_t TASKS_LIST_OFFSET = 0x7e0;  // NOT 0x7e8!
const uint32_t TASK_STRUCT_SIZE = 9088;
const uint32_t PAGE_SIZE = 4096;

// Known process names to look for
const char* KNOWN_PROCESSES[] = {
    "systemd", "init", "kthreadd", "kworker", "ksoftirqd", "migration",
    "rcu_", "sshd", "bash", "NetworkManager", "dbus", "cron", "systemd-"
};

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
                     std::string& nameOut, uint32_t& pidOut, bool& hasValidList) {
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

    // Validate linked list
    hasValidList = validateLinkedList(mem, offset);

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

    std::cout << "Scanning for REAL processes with kernel pointer validation..." << std::endl;
    std::cout << "File: " << memFile << " (" << fileSize / (1024*1024) << " MB)" << std::endl;
    std::cout << std::endl;

    const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};
    const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};

    std::vector<uint64_t> allOffsets;
    for (auto o : SLAB_OFFSETS) allOffsets.push_back(o);
    for (auto o : PAGE_STRADDLE_OFFSETS) allOffsets.push_back(o);

    int foundWithValidList = 0;
    int foundWithoutValidList = 0;
    int displayCount = 0;
    std::set<std::string> uniqueNames;

    for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
        if (pageStart % (1024 * 1024 * 1024) == 0) {
            std::cout << "  Scanning " << pageStart / (1024 * 1024)
                      << "MB... (valid lists: " << foundWithValidList
                      << ", no list: " << foundWithoutValidList << ")\r" << std::flush;
        }

        for (auto slabOffset : allOffsets) {
            uint64_t offset = pageStart + slabOffset;
            std::string name;
            uint32_t pid;
            bool hasValidList;

            if (checkTaskStruct(mem, offset, fileSize, name, pid, hasValidList)) {
                uniqueNames.insert(name);

                if (hasValidList) {
                    foundWithValidList++;

                    // Always display processes with valid lists
                    if (displayCount < 20) {
                        displayCount++;
                        std::cout << "\n=== Process with VALID linked list ===" << std::endl;
                        std::cout << "  Offset: 0x" << std::hex << offset << std::dec << std::endl;
                        std::cout << "  PID: " << pid << std::endl;
                        std::cout << "  Name: '" << name << "'" << std::endl;

                        // Show linked list pointers
                        uint64_t* nextPtr = (uint64_t*)(mem + offset + TASKS_LIST_OFFSET);
                        uint64_t* prevPtr = (uint64_t*)(mem + offset + TASKS_LIST_OFFSET + 8);
                        std::cout << "  tasks.next: 0x" << std::hex << *nextPtr << std::dec;
                        if (isKernelPointer(*nextPtr)) std::cout << " (kernel ptr)";
                        std::cout << std::endl;
                        std::cout << "  tasks.prev: 0x" << std::hex << *prevPtr << std::dec;
                        if (isKernelPointer(*prevPtr)) std::cout << " (kernel ptr)";
                        std::cout << std::endl;

                        // Check if it's a known process
                        bool isKnown = false;
                        for (auto known : KNOWN_PROCESSES) {
                            if (name.find(known) != std::string::npos) {
                                isKnown = true;
                                break;
                            }
                        }
                        if (isKnown) {
                            std::cout << "  *** RECOGNIZED SYSTEM PROCESS ***" << std::endl;
                        }
                    }
                } else {
                    foundWithoutValidList++;
                }
            }
        }
    }

    std::cout << "\n\n=== SUMMARY ===" << std::endl;
    std::cout << "Processes with valid kernel linked lists: " << foundWithValidList << std::endl;
    std::cout << "Processes without valid lists: " << foundWithoutValidList << std::endl;
    std::cout << "Unique process names found: " << uniqueNames.size() << std::endl;

    if (uniqueNames.size() <= 100) {
        std::cout << "\nUnique names:" << std::endl;
        for (const auto& name : uniqueNames) {
            std::cout << "  '" << name << "'";
            bool isKnown = false;
            for (auto known : KNOWN_PROCESSES) {
                if (name.find(known) != std::string::npos) {
                    isKnown = true;
                    break;
                }
            }
            if (isKnown) std::cout << " <-- SYSTEM PROCESS";
            std::cout << std::endl;
        }
    }

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}