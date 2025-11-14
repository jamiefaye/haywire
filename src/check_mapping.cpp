/**
 * Check our memory mapping more carefully
 */

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>

void dumpBytes(uint8_t* mem, uint64_t offset, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && i % 16 == 0) std::cout << std::endl;
        std::cout << std::hex << std::setfill('0') << std::setw(2)
                  << (int)mem[offset + i] << " ";
    }
    std::cout << std::dec << std::endl;
}

int main() {
    const char* memFile = "/tmp/haywire-vm-mem";

    // Get file stats
    struct stat st;
    if (stat(memFile, &st) != 0) {
        std::cerr << "Failed to stat " << memFile << std::endl;
        return 1;
    }

    std::cout << "File: " << memFile << std::endl;
    std::cout << "Size from stat: " << st.st_size << " bytes (0x" << std::hex << st.st_size << std::dec << ")" << std::endl;

    int fd = open(memFile, O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << memFile << std::endl;
        return 1;
    }

    // Check size via lseek
    off_t fileSize = lseek(fd, 0, SEEK_END);
    std::cout << "Size from lseek: " << fileSize << " bytes (0x" << std::hex << fileSize << std::dec << ")" << std::endl;

    // Map the file
    void* memBase = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to mmap" << std::endl;
        close(fd);
        return 1;
    }

    std::cout << "mmap base address: " << memBase << std::endl;
    std::cout << std::endl;

    uint8_t* mem = (uint8_t*)memBase;

    // Test direct reading vs mmap at offset 0x40000000
    std::cout << "=== Testing offset 0x40000000 ===" << std::endl;

    // Via mmap
    std::cout << "Via mmap:" << std::endl;
    dumpBytes(mem, 0x40000000, 32);

    // Via pread
    uint8_t buffer[32];
    ssize_t bytesRead = pread(fd, buffer, 32, 0x40000000);
    if (bytesRead == 32) {
        std::cout << "\nVia pread:" << std::endl;
        for (int i = 0; i < 32; i++) {
            if (i > 0 && i % 16 == 0) std::cout << std::endl;
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << (int)buffer[i] << " ";
        }
        std::cout << std::dec << std::endl;
    } else {
        std::cout << "pread failed or incomplete: " << bytesRead << std::endl;
    }

    // Check page boundaries around 0x1000
    std::cout << "\n=== Checking around offset 0x1000 ===" << std::endl;

    for (uint64_t offset = 0x0; offset <= 0x3000; offset += 0x1000) {
        std::cout << "\nOffset 0x" << std::hex << offset << std::dec << ":" << std::endl;

        // Check if page is all zeros
        bool allZeros = true;
        for (int i = 0; i < 4096; i++) {
            if (mem[offset + i] != 0) {
                allZeros = false;
                break;
            }
        }

        if (allZeros) {
            std::cout << "  Page is all zeros" << std::endl;
        } else {
            std::cout << "  Page has data. First 32 bytes:" << std::endl;
            std::cout << "  ";
            dumpBytes(mem, offset, 32);
        }
    }

    // Let's also check if we can read beyond 4GB boundary correctly
    std::cout << "\n=== Testing large offsets ===" << std::endl;

    uint64_t test_offsets[] = {
        0xFFFFFF00,    // Just before 4GB
        0x100000000,   // Exactly 4GB
        0x100000100,   // Just after 4GB
    };

    for (auto offset : test_offsets) {
        if (offset + 32 <= fileSize) {
            std::cout << "\nOffset 0x" << std::hex << offset << std::dec << ":" << std::endl;
            dumpBytes(mem, offset, 32);
        } else {
            std::cout << "\nOffset 0x" << std::hex << offset << std::dec << ": Beyond file size" << std::endl;
        }
    }

    munmap(memBase, fileSize);
    close(fd);

    return 0;
}