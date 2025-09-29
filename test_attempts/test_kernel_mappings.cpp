#include <iostream>
#include <iomanip>
#include <set>
#include <map>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void* memBase = nullptr;
size_t memorySize = 0;

// Stats
std::map<int, std::set<uint64_t>> mapped_pages_by_pgd;
std::set<uint64_t> all_kernel_mapped_pages;
size_t huge_1gb_count = 0;
size_t huge_2mb_count = 0;
size_t regular_4kb_count = 0;

void WalkPTE(uint64_t pteBase, int pgdIndex) {
    // Walk 512 PTE entries
    for (int i = 0; i < 512; i++) {
        uint64_t pteOffset = (pteBase - 0x40000000) + (i * 8);
        if (pteOffset + 8 > memorySize) continue;

        uint64_t pte = *(uint64_t*)((uint8_t*)memBase + pteOffset);
        if (!(pte & 1)) continue;  // Not valid

        uint64_t pa = pte & 0x0000FFFFFFFFF000ULL;
        mapped_pages_by_pgd[pgdIndex].insert(pa);
        all_kernel_mapped_pages.insert(pa);
        regular_4kb_count++;
    }
}

void WalkPMD(uint64_t pmdBase, int pgdIndex) {
    // Walk 512 PMD entries
    for (int i = 0; i < 512; i++) {
        uint64_t pmdOffset = (pmdBase - 0x40000000) + (i * 8);
        if (pmdOffset + 8 > memorySize) continue;

        uint64_t pmd = *(uint64_t*)((uint8_t*)memBase + pmdOffset);
        if (!(pmd & 1)) continue;  // Not valid

        if (!(pmd & 2)) {
            // 2MB huge page
            uint64_t pa = pmd & 0x0000FFFFFFE00000ULL;
            for (uint64_t p = pa; p < pa + 0x200000; p += 0x1000) {
                mapped_pages_by_pgd[pgdIndex].insert(p);
                all_kernel_mapped_pages.insert(p);
            }
            huge_2mb_count++;
        } else {
            // Points to PTE table
            uint64_t pteBase = pmd & 0x0000FFFFFFFFF000ULL;
            if (pteBase >= 0x40000000 && pteBase < 0x40000000 + memorySize) {
                WalkPTE(pteBase, pgdIndex);
            }
        }
    }
}

void WalkPUD(uint64_t pudBase, int pgdIndex) {
    // Walk 512 PUD entries
    for (int i = 0; i < 512; i++) {
        uint64_t pudOffset = (pudBase - 0x40000000) + (i * 8);
        if (pudOffset + 8 > memorySize) continue;

        uint64_t pud = *(uint64_t*)((uint8_t*)memBase + pudOffset);
        if (!(pud & 1)) continue;  // Not valid

        if (!(pud & 2)) {
            // 1GB huge page
            uint64_t pa = pud & 0x0000FFFFC0000000ULL;
            for (uint64_t p = pa; p < pa + 0x40000000; p += 0x1000) {
                mapped_pages_by_pgd[pgdIndex].insert(p);
                all_kernel_mapped_pages.insert(p);
            }
            huge_1gb_count++;
        } else {
            // Points to PMD table
            uint64_t pmdBase = pud & 0x0000FFFFFFFFF000ULL;
            if (pmdBase >= 0x40000000 && pmdBase < 0x40000000 + memorySize) {
                WalkPMD(pmdBase, pgdIndex);
            }
        }
    }
}

int main() {
    // Open memory file
    int fd = open("/tmp/haywire-vm-mem", O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open memory file\n";
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    memorySize = st.st_size;

    memBase = mmap(nullptr, memorySize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to mmap\n";
        close(fd);
        return 1;
    }

    std::cout << "Memory mapped: " << (memorySize / 1024 / 1024) << " MB\n\n";

    // Hardcoded swapper PGD (from QMP)
    const uint64_t SWAPPER_PGD = 0x136deb000;

    std::cout << "Walking kernel page tables from swapper PGD: 0x"
              << std::hex << SWAPPER_PGD << std::dec << "\n";
    std::cout << "Checking PGD entries 256-511 (kernel space)\n\n";

    // Walk kernel PGD entries (256-511)
    for (int pgdIndex = 256; pgdIndex < 512; pgdIndex++) {
        uint64_t pgdOffset = (SWAPPER_PGD - 0x40000000) + (pgdIndex * 8);
        if (pgdOffset + 8 > memorySize) continue;

        uint64_t pgdEntry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);
        if (!(pgdEntry & 1)) continue;  // Not valid

        if (!(pgdEntry & 2)) {
            std::cout << "PGD[" << pgdIndex << "] is a huge page (unexpected!)\n";
            continue;
        }

        // Points to PUD table
        uint64_t pudBase = pgdEntry & 0x0000FFFFFFFFF000ULL;
        if (pudBase >= 0x40000000 && pudBase < 0x40000000 + memorySize) {
            WalkPUD(pudBase, pgdIndex);
            if (!mapped_pages_by_pgd[pgdIndex].empty()) {
                std::cout << "PGD[" << pgdIndex << "] maps "
                          << mapped_pages_by_pgd[pgdIndex].size() << " pages\n";
            }
        }
    }

    std::cout << "\n=== Kernel Mapping Statistics ===\n";
    std::cout << "Total kernel-mapped pages: " << all_kernel_mapped_pages.size() << "\n";
    std::cout << "Total mapped memory: "
              << (all_kernel_mapped_pages.size() * 4096 / 1024 / 1024) << " MB\n";
    std::cout << "1GB huge pages: " << huge_1gb_count << "\n";
    std::cout << "2MB huge pages: " << huge_2mb_count << "\n";
    std::cout << "4KB regular pages: " << regular_4kb_count << "\n";

    // Check if our task_struct PAs are in kernel-mapped regions
    std::cout << "\n=== Checking Task_struct PAs ===\n";
    std::vector<uint64_t> task_pas = {
        0x42238000, 0x4223c700, 0x42252380, 0x42254700
    };

    for (uint64_t pa : task_pas) {
        uint64_t page_pa = pa & ~0xFFF;
        if (all_kernel_mapped_pages.count(page_pa)) {
            std::cout << "PA 0x" << std::hex << pa << " IS kernel-mapped ";
            // Find which PGD entry maps it
            for (auto& [pgd, pages] : mapped_pages_by_pgd) {
                if (pages.count(page_pa)) {
                    std::cout << "(via PGD[" << std::dec << pgd << "])";
                    break;
                }
            }
            std::cout << "\n";
        } else {
            std::cout << "PA 0x" << std::hex << pa << " is NOT kernel-mapped!\n";
        }
    }

    // Show PA ranges that ARE kernel-mapped
    std::cout << "\n=== Kernel-Mapped PA Ranges ===\n";
    if (!all_kernel_mapped_pages.empty()) {
        uint64_t range_start = *all_kernel_mapped_pages.begin();
        uint64_t range_end = range_start;

        for (uint64_t pa : all_kernel_mapped_pages) {
            if (pa == range_end + 0x1000) {
                range_end = pa;
            } else {
                if (range_end - range_start >= 0x100000) {  // Show ranges >= 1MB
                    std::cout << "0x" << std::hex << range_start << " - 0x" << range_end
                              << " (" << std::dec << (range_end - range_start + 0x1000) / 1024 / 1024
                              << " MB)\n";
                }
                range_start = pa;
                range_end = pa;
            }
        }
    }

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}