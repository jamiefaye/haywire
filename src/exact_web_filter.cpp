/**
 * Exact implementation of web version's filtering
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
#include <regex>
#include <algorithm>

// From web version KernelConstants - EXACT VALUES
const uint32_t PID_OFFSET = 0x750;
const uint32_t COMM_OFFSET = 0x970;
const uint32_t MM_OFFSET = 0x6d0;
const uint32_t TASKS_LIST_OFFSET = 0x7e0;
const uint32_t TASK_STRUCT_SIZE = 9088;
const uint32_t PAGE_SIZE = 4096;

// Known process names
const std::vector<std::string> KNOWN_PROCESSES = {
    "systemd", "init", "kthreadd", "kworker", "ksoftirqd", "migration",
    "rcu_", "sshd", "bash", "NetworkManager", "dbus", "cron", "systemd-"
};

bool isKernelPointer(uint64_t ptr) {
    return (ptr >> 48) == 0xFFFF;
}

bool validateLinkedList(uint8_t* mem, uint64_t offset) {
    uint64_t listOffset = offset + TASKS_LIST_OFFSET;
    uint64_t* nextPtr = (uint64_t*)(mem + listOffset);
    uint64_t* prevPtr = (uint64_t*)(mem + listOffset + 8);

    if (*nextPtr == 0 || *prevPtr == 0) return false;
    if (!isKernelPointer(*nextPtr) || !isKernelPointer(*prevPtr)) return false;

    return true;
}

int countKernelPointers(uint8_t* mem, uint64_t offset) {
    int count = 0;
    // Check first 512 bytes for kernel pointers
    for (uint64_t checkOffset = offset; checkOffset < offset + 512; checkOffset += 8) {
        uint64_t* ptr = (uint64_t*)(mem + checkOffset);
        if (isKernelPointer(*ptr)) {
            count++;
            if (count >= 10) break;  // Enough for max score
        }
    }
    return count;
}

int countCaseTransitions(const std::string& str) {
    int transitions = 0;
    for (size_t i = 1; i < str.length(); i++) {
        bool prevIsUpper = (str[i-1] >= 'A' && str[i-1] <= 'Z');
        bool currIsUpper = (str[i] >= 'A' && str[i] <= 'Z');
        bool prevIsLower = (str[i-1] >= 'a' && str[i-1] <= 'z');
        bool currIsLower = (str[i] >= 'a' && str[i] <= 'z');

        if ((prevIsUpper && currIsLower) || (prevIsLower && currIsUpper)) {
            transitions++;
        }
    }
    return transitions;
}

bool isPrintableString(const std::string& str) {
    // Exact implementation from web version
    if (str.length() < 2 || str.length() > 15) return false;

    int alphaNum = 0;
    int special = 0;
    int invalid = 0;

    for (char c : str) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            alphaNum++;
        } else if (c == '/' || c == '-' || c == '_' || c == ':' || c == '.' || c == '[' || c == ']') {
            special++;
        } else if (c < ' ' || c > '~') {
            invalid++;
        }
    }

    // Must have mostly alphanumeric characters
    return alphaNum >= 2 && invalid == 0 && (alphaNum + special) >= str.length() * 0.8;
}

bool checkTaskStruct(uint8_t* mem, uint64_t offset, uint64_t fileSize,
                     std::string& nameOut, uint32_t& pidOut) {
    if (offset + TASK_STRUCT_SIZE > fileSize) return false;

    // Check PID
    uint32_t pid = *(uint32_t*)(mem + offset + PID_OFFSET);
    if (!pid || pid < 1 || pid > 32768) {
        return false;
    }

    // Read comm (process name)
    uint8_t* comm = mem + offset + COMM_OFFSET;

    // Find null terminator
    int nullIdx = -1;
    for (int i = 0; i < 16; i++) {
        if (comm[i] == 0) {
            nullIdx = i;
            break;
        }
    }

    if (nullIdx == 0 || nullIdx > 15) {
        return false;
    }

    if (nullIdx == -1) nullIdx = 16;

    // Check for printable ASCII
    for (int i = 0; i < nullIdx; i++) {
        if (comm[i] < 0x20 || comm[i] > 0x7E) {
            return false;
        }
    }

    std::string name((char*)comm, nullIdx);

    // Check if it's a known process
    bool isKnown = false;
    for (const auto& known : KNOWN_PROCESSES) {
        if (name.find(known) != std::string::npos) {
            isKnown = true;
            break;
        }
    }

    // Check if name is valid - be ULTRA strict (from web version)
    if (!isPrintableString(name)) {
        return false;
    }

    // Reject very short names unless known
    if (name.length() < 3 && !isKnown) {
        return false;
    }

    // Must match pattern: ^[a-zA-Z\/][a-zA-Z0-9\-_\/\[\]:\.\$]*$
    std::regex namePattern("^[a-zA-Z/][a-zA-Z0-9\\-_/\\[\\]:\\.\\$]*$");
    if (!std::regex_match(name, namePattern)) {
        return false;
    }

    // Require at least 2 alphanumeric characters
    int alphaCount = 0;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            alphaCount++;
        }
    }
    if (alphaCount < 2) {
        return false;
    }

    // Check case transitions for mixed-case names
    if (!isKnown && name.length() > 2) {
        int upperCount = 0, lowerCount = 0;
        for (char c : name) {
            if (c >= 'A' && c <= 'Z') upperCount++;
            if (c >= 'a' && c <= 'z') lowerCount++;
        }

        if (upperCount > 0 && lowerCount > 0) {
            int transitions = countCaseTransitions(name);
            if (transitions > name.length() / 2) {
                return false;  // Too many case transitions
            }
        }
    }

    // Check kernel pointer count FIRST - web version requires at least 3
    int kernelPtrCount = countKernelPointers(mem, offset);
    if (kernelPtrCount < 3) {
        return false;  // Web version early rejects if < 3 kernel pointers
    }

    // Calculate validity score
    int validityScore = 0;

    if (isKnown) validityScore += 3;

    bool hasValidList = validateLinkedList(mem, offset);
    if (hasValidList) validityScore += 2;

    if (kernelPtrCount >= 5) validityScore += 2;
    if (kernelPtrCount >= 10) validityScore += 1;

    // Check mm pointer
    uint64_t mmPtr = *(uint64_t*)(mem + offset + MM_OFFSET);
    if (mmPtr == 0 || isKernelPointer(mmPtr)) {
        validityScore += 1;
    }

    // Require score >= 3
    if (validityScore < 3) {
        return false;
    }

    // Additional mm_struct validation
    if (mmPtr != 0) {
        // For user processes, mm should be kernel VA or in guest RAM range
        if (!isKernelPointer(mmPtr)) {
            // Check if it's in guest RAM range (0x40000000 to 0x1C0000000 for 6GB)
            // In file, this is 0x0 to 0x180000000
            // Physical addresses in guest RAM would be 0x40000000 to 0x1C0000000
            if (mmPtr < 0x40000000 || mmPtr >= 0x1C0000000) {
                return false;  // Neither kernel VA nor plausible physical address
            }
        }
    }

    // All checks passed
    pidOut = pid;
    nameOut = name;
    return true;
}

int main() {
    const char* memFile = "/tmp/haywire-vm-mem";  // New snapshot from live VM with swapper PGD 0x136DEB000

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

    std::cout << "Using EXACT web version filtering..." << std::endl;
    std::cout << std::endl;

    const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};
    const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};

    std::vector<uint64_t> allOffsets;
    for (auto o : SLAB_OFFSETS) allOffsets.push_back(o);
    for (auto o : PAGE_STRADDLE_OFFSETS) allOffsets.push_back(o);

    std::map<std::string, std::vector<uint32_t>> processesByName;
    std::set<uint32_t> uniquePids;
    int kernelThreadCount = 0;
    int userProcessCount = 0;

    for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
        if (pageStart % (1024 * 1024 * 1024) == 0) {
            std::cout << "  Scanning " << pageStart / (1024 * 1024)
                      << "MB...\r" << std::flush;
        }

        for (auto slabOffset : allOffsets) {
            uint64_t offset = pageStart + slabOffset;
            std::string name;
            uint32_t pid;

            if (checkTaskStruct(mem, offset, fileSize, name, pid)) {
                // Deduplicate by PID
                if (uniquePids.find(pid) == uniquePids.end()) {
                    uniquePids.insert(pid);
                    processesByName[name].push_back(pid);

                    // Check if kernel thread (mm == 0)
                    uint64_t mmPtr = *(uint64_t*)(mem + offset + MM_OFFSET);
                    if (mmPtr == 0) {
                        kernelThreadCount++;
                    } else {
                        userProcessCount++;
                    }
                }
            }
        }
    }

    // Apply "realProcesses" filtering from web version
    std::map<std::string, std::vector<uint32_t>> realProcesses;
    std::set<uint32_t> realPids;
    int realKernelCount = 0;
    int realUserCount = 0;

    for (const auto& entry : processesByName) {
        const std::string& name = entry.first;

        // Check if it's a known process
        bool isKnown = false;
        for (const auto& known : KNOWN_PROCESSES) {
            if (name.find(known) != std::string::npos) {
                isKnown = true;
                break;
            }
        }

        // For each PID with this name, check mm validity
        for (uint32_t pid : entry.second) {
            // Find the task_struct for this PID to check mm
            bool foundValid = false;
            for (uint64_t pageStart = 0; pageStart < fileSize; pageStart += PAGE_SIZE) {
                for (auto slabOffset : allOffsets) {
                    uint64_t offset = pageStart + slabOffset;
                    if (offset + TASK_STRUCT_SIZE > fileSize) continue;

                    uint32_t checkPid = *(uint32_t*)(mem + offset + PID_OFFSET);
                    if (checkPid == pid) {
                        uint64_t mmPtr = *(uint64_t*)(mem + offset + MM_OFFSET);
                        bool hasValidMm = (mmPtr == 0 || mmPtr >= 0xffff000000000000ULL);

                        // Must be either known OR (has valid mm AND matches pattern)
                        if (isKnown || hasValidMm) {
                            realProcesses[name].push_back(pid);
                            realPids.insert(pid);
                            if (mmPtr == 0) realKernelCount++;
                            else realUserCount++;
                            foundValid = true;
                            break;
                        }
                    }
                }
                if (foundValid) break;
            }
        }
    }

    std::cout << "\n\n=== PROCESSES WITH EXACT WEB FILTERING ===" << std::endl;
    std::cout << "Found " << uniquePids.size() << " unique PIDs, "
              << processesByName.size() << " unique names" << std::endl;
    std::cout << "  Kernel threads (mm==0): " << kernelThreadCount << std::endl;
    std::cout << "  User processes (mm!=0): " << userProcessCount << std::endl;
    std::cout << std::endl;

    std::cout << "=== AFTER 'realProcesses' FILTERING ===" << std::endl;
    std::cout << "Found " << realPids.size() << " real PIDs, "
              << realProcesses.size() << " unique names" << std::endl;
    std::cout << "  Real kernel threads: " << realKernelCount << std::endl;
    std::cout << "  Real user processes: " << realUserCount << std::endl;
    std::cout << std::endl;

    // Show REAL processes only
    std::cout << "=== REAL PROCESSES LIST ===" << std::endl;
    for (const auto& entry : realProcesses) {
        std::cout << std::setw(20) << std::left << entry.first << " : ";

        // Show PIDs
        for (size_t i = 0; i < entry.second.size() && i < 5; i++) {
            std::cout << entry.second[i];
            if (i < entry.second.size() - 1 && i < 4) std::cout << ", ";
        }
        if (entry.second.size() > 5) {
            std::cout << " ... (" << entry.second.size() << " total)";
        }

        // Mark system processes
        bool isSystem = false;
        for (const auto& known : KNOWN_PROCESSES) {
            if (entry.first.find(known) != std::string::npos) {
                isSystem = true;
                break;
            }
        }
        if (isSystem) std::cout << " <-- SYSTEM";

        std::cout << std::endl;
    }

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}