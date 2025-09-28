/**
 * Dump bytes at specific memory locations for comparison with web version
 */

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

void dumpLocation(uint8_t* mem, uint64_t offset, size_t fileSize) {
    std::cout << "\n=== Offset 0x" << std::hex << offset << " ===" << std::dec << std::endl;

    if (offset >= fileSize) {
        std::cout << "  BEYOND FILE SIZE (file ends at 0x" << std::hex << fileSize << ")" << std::dec << std::endl;
        return;
    }

    // Show 32 bytes
    std::cout << "  Hex bytes: ";
    for (int i = 0; i < 32; i++) {
        if (offset + i < fileSize) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << (int)mem[offset + i] << " ";
            if (i == 15) std::cout << "\n              ";
        }
    }
    std::cout << std::dec << std::endl;

    // Show as ASCII
    std::cout << "  ASCII:     ";
    for (int i = 0; i < 32; i++) {
        if (offset + i < fileSize) {
            uint8_t c = mem[offset + i];
            if (c >= 0x20 && c <= 0x7E) {
                std::cout << (char)c;
            } else {
                std::cout << ".";
            }
            if (i == 15) std::cout << "\n              ";
        }
    }
    std::cout << std::endl;

    // Show as uint32_t and uint64_t
    if (offset + 4 <= fileSize) {
        uint32_t* u32 = (uint32_t*)(mem + offset);
        std::cout << "  As uint32_t: " << *u32 << " (0x" << std::hex << *u32 << ")" << std::dec << std::endl;
    }
    if (offset + 8 <= fileSize) {
        uint64_t* u64 = (uint64_t*)(mem + offset);
        std::cout << "  As uint64_t: " << *u64 << " (0x" << std::hex << *u64 << ")" << std::dec << std::endl;
    }
}

int main() {
    const char* memFile = "haywire-vm-mem";  // Use snapshot

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

    std::cout << "Memory file: " << memFile << std::endl;
    std::cout << "File size: 0x" << std::hex << fileSize << " (" << std::dec << fileSize / (1024*1024) << " MB)" << std::endl;
    std::cout << "Base address: " << memBase << std::endl;

    // Important note about addressing
    std::cout << "\n=== ADDRESSING SCHEME ===" << std::endl;
    std::cout << "File offset 0x0 = Physical address 0x40000000 (GUEST_RAM_START)" << std::endl;
    std::cout << "File offset 0x100000000 = Physical address 0x140000000" << std::endl;

    // Dump specific locations
    dumpLocation(mem, 0x0, fileSize);                  // Start of file
    dumpLocation(mem, 0x1000, fileSize);               // Page 1
    dumpLocation(mem, 0x40000000, fileSize);           // 1GB mark (would be PA 0x80000000)
    dumpLocation(mem, 0x80000000, fileSize);           // 2GB mark (would be PA 0xC0000000)
    dumpLocation(mem, 0xC0000000, fileSize);           // 3GB mark (would be PA 0x100000000)
    dumpLocation(mem, 0x100000000, fileSize);          // 4GB mark (would be PA 0x140000000)
    dumpLocation(mem, 0x140000000, fileSize);          // 5GB mark (would be PA 0x180000000)
    dumpLocation(mem, 0x180000000, fileSize);          // 6GB mark (would be PA 0x1C0000000)

    // Also dump where we found PID=144 earlier
    dumpLocation(mem, 0x7750, fileSize);               // Where we saw PID=144

    // Dump at common kernel structure locations
    std::cout << "\n=== Checking kernel structure locations ===" << std::endl;
    dumpLocation(mem, 0xB0000000, fileSize);           // ~2.75GB (PA 0xF0000000)
    dumpLocation(mem, 0x130000000, fileSize);          // ~4.75GB (PA 0x170000000)

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}