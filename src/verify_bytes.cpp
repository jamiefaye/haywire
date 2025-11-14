/**
 * Verify that we're reading the same bytes as the web version
 */

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

void dumpBytes(uint8_t* mem, size_t offset, size_t len, const char* label) {
    std::cout << label << " at offset 0x" << std::hex << offset << std::dec << ":" << std::endl;
    std::cout << "  Hex: ";
    for (size_t i = 0; i < len; i++) {
        std::cout << std::hex << std::setfill('0') << std::setw(2)
                  << (int)mem[offset + i] << " ";
    }
    std::cout << std::dec << std::endl;

    std::cout << "  ASCII: ";
    for (size_t i = 0; i < len; i++) {
        uint8_t c = mem[offset + i];
        if (c >= 0x20 && c <= 0x7E) {
            std::cout << (char)c;
        } else {
            std::cout << ".";
        }
    }
    std::cout << std::endl;

    // Show as uint32_t at key offsets
    if (len >= 4) {
        uint32_t* u32 = (uint32_t*)(mem + offset);
        std::cout << "  As uint32: " << *u32 << " (0x" << std::hex << *u32 << std::dec << ")" << std::endl;
    }
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

    std::cout << "Memory file mapped, size: " << fileSize << " bytes ("
              << fileSize / (1024*1024) << " MB)" << std::endl;
    std::cout << std::endl;

    // Test some specific offsets that the web version finds processes at
    // Let's check the common SLAB offsets on early pages
    const uint64_t PAGE_SIZE = 4096;
    const uint64_t SLAB_OFFSETS[] = {0x0, 0x2380, 0x4700};
    const uint64_t PAGE_STRADDLE_OFFSETS[] = {0x0, 0x380, 0x700};

    std::cout << "=== Checking first few pages for task_structs ===" << std::endl;

    // Check first 10 pages
    for (int page = 0; page < 10; page++) {
        uint64_t pageStart = page * PAGE_SIZE;
        bool foundSomething = false;

        // Check all offsets
        for (auto offset : SLAB_OFFSETS) {
            uint64_t addr = pageStart + offset;
            if (addr + 0x1000 > fileSize) continue;

            // Read PID at offset 0x750
            uint32_t pid = *(uint32_t*)(mem + addr + 0x750);
            if (pid > 0 && pid <= 32768) {
                if (!foundSomething) {
                    std::cout << "\nPage " << page << " (0x" << std::hex << pageStart << std::dec << "):" << std::endl;
                    foundSomething = true;
                }

                std::cout << "  SLAB offset 0x" << std::hex << offset << std::dec << " (total: 0x"
                          << std::hex << addr << std::dec << "):" << std::endl;

                // Dump PID bytes
                dumpBytes(mem, addr + 0x750, 4, "    PID");

                // Dump comm bytes
                dumpBytes(mem, addr + 0x970, 16, "    Comm");

                // Dump tasks.next and tasks.prev
                dumpBytes(mem, addr + 0x7e8, 8, "    tasks.next");
                dumpBytes(mem, addr + 0x7f0, 8, "    tasks.prev");
            }
        }

        for (auto offset : PAGE_STRADDLE_OFFSETS) {
            uint64_t addr = pageStart + offset;
            if (addr + 0x1000 > fileSize) continue;

            // Read PID at offset 0x750
            uint32_t pid = *(uint32_t*)(mem + addr + 0x750);
            if (pid > 0 && pid <= 32768) {
                if (!foundSomething) {
                    std::cout << "\nPage " << page << " (0x" << std::hex << pageStart << std::dec << "):" << std::endl;
                    foundSomething = true;
                }

                std::cout << "  PAGE_STRADDLE offset 0x" << std::hex << offset << std::dec
                          << " (total: 0x" << std::hex << addr << std::dec << "):" << std::endl;

                // Dump PID bytes
                dumpBytes(mem, addr + 0x750, 4, "    PID");

                // Dump comm bytes
                dumpBytes(mem, addr + 0x970, 16, "    Comm");

                // Dump tasks.next and tasks.prev
                dumpBytes(mem, addr + 0x7e8, 8, "    tasks.next");
                dumpBytes(mem, addr + 0x7f0, 8, "    tasks.prev");
            }
        }
    }

    // Also let's check a specific offset where we know systemd might be
    std::cout << "\n=== Looking for specific patterns ===" << std::endl;
    std::cout << "Searching for task_structs with recognizable names in first 100MB..." << std::endl;

    int found = 0;
    for (uint64_t pageStart = 0; pageStart < 100*1024*1024 && found < 5; pageStart += PAGE_SIZE) {
        std::vector<uint64_t> allOffsets;
        for (auto o : SLAB_OFFSETS) allOffsets.push_back(o);
        for (auto o : PAGE_STRADDLE_OFFSETS) allOffsets.push_back(o);

        for (auto offset : allOffsets) {
            uint64_t addr = pageStart + offset;
            if (addr + 0x1000 > fileSize) continue;

            // Check comm field
            char* comm = (char*)(mem + addr + 0x970);

            // Look for a null terminator within 16 bytes
            int nullIdx = -1;
            for (int i = 0; i < 16; i++) {
                if (comm[i] == 0) {
                    nullIdx = i;
                    break;
                }
            }

            // Check if it's a recognizable name
            if (nullIdx > 0 && nullIdx <= 15) {
                bool recognizable = false;
                std::string name(comm, nullIdx);

                // Check for common process names
                if (name == "systemd" || name == "init" || name == "sshd" ||
                    name == "bash" || name == "kworker" || name.find("systemd") != std::string::npos) {
                    recognizable = true;
                }

                if (recognizable) {
                    found++;
                    std::cout << "\nFound '" << name << "' at offset 0x" << std::hex << addr << std::dec << std::endl;

                    // Dump the whole area
                    dumpBytes(mem, addr + 0x750, 4, "  PID");
                    dumpBytes(mem, addr + 0x970, 16, "  Comm");
                    dumpBytes(mem, addr + 0x7e8, 8, "  tasks.next");
                    dumpBytes(mem, addr + 0x7f0, 8, "  tasks.prev");
                    dumpBytes(mem, addr + 0x6d0, 8, "  mm_struct");
                }
            }
        }
    }

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}