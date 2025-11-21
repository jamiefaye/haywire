#include <iostream>
#include <iomanip>
#include <set>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void* memBase = nullptr;
size_t memorySize = 0;

std::set<uint64_t> pgd0_mapped_pages;
size_t huge_1gb_count = 0;
size_t huge_2mb_count = 0;
size_t regular_4kb_count = 0;

void WalkPTE(uint64_t pteBase) {
    for (int i = 0; i < 512; i++) {
        uint64_t pteOffset = (pteBase - 0x40000000) + (i * 8);
        if (pteOffset + 8 > memorySize) continue;

        uint64_t pte = *(uint64_t*)((uint8_t*)memBase + pteOffset);
        if (!(pte & 1)) continue;

        uint64_t pa = pte & 0x0000FFFFFFFFF000ULL;
        pgd0_mapped_pages.insert(pa);
        regular_4kb_count++;
    }
}

void WalkPMD(uint64_t pmdBase) {
    for (int i = 0; i < 512; i++) {
        uint64_t pmdOffset = (pmdBase - 0x40000000) + (i * 8);
        if (pmdOffset + 8 > memorySize) continue;

        uint64_t pmd = *(uint64_t*)((uint8_t*)memBase + pmdOffset);
        if (!(pmd & 1)) continue;

        if (!(pmd & 2)) {
            // 2MB huge page
            uint64_t pa = pmd & 0x0000FFFFFFE00000ULL;
            std::cout << "  PMD[" << i << "] is 2MB huge page at PA 0x"
                      << std::hex << pa << std::dec << "\n";
            for (uint64_t p = pa; p < pa + 0x200000; p += 0x1000) {
                pgd0_mapped_pages.insert(p);
            }
            huge_2mb_count++;
        } else {
            uint64_t pteBase = pmd & 0x0000FFFFFFFFF000ULL;
            if (pteBase >= 0x40000000 && pteBase < 0x40000000 + memorySize) {
                WalkPTE(pteBase);
            }
        }
    }
}

void WalkPUD(uint64_t pudBase) {
    std::cout << "Walking PUD at PA 0x" << std::hex << pudBase << std::dec << "\n";

    for (int i = 0; i < 512; i++) {
        uint64_t pudOffset = (pudBase - 0x40000000) + (i * 8);
        if (pudOffset + 8 > memorySize) continue;

        uint64_t pud = *(uint64_t*)((uint8_t*)memBase + pudOffset);
        if (!(pud & 1)) continue;

        if (!(pud & 2)) {
            // 1GB huge page!
            uint64_t pa = pud & 0x0000FFFFC0000000ULL;
            std::cout << "  PUD[" << i << "] is 1GB huge page at PA 0x"
                      << std::hex << pa << std::dec;

            // Check if this covers our task_struct region
            if (pa <= 0x42238000 && pa + 0x40000000 > 0x42254700) {
                std::cout << " <-- COVERS TASK_STRUCTS!";
            }
            std::cout << "\n";

            // Add all pages in this 1GB region
            for (uint64_t p = pa; p < pa + 0x40000000; p += 0x1000) {
                pgd0_mapped_pages.insert(p);
            }
            huge_1gb_count++;
        } else {
            uint64_t pmdBase = pud & 0x0000FFFFFFFFF000ULL;
            if (pmdBase >= 0x40000000 && pmdBase < 0x40000000 + memorySize) {
                WalkPMD(pmdBase);
            }
        }
    }
}

int main() {
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

    const uint64_t SWAPPER_PGD = 0x136deb000;

    std::cout << "=== Checking PGD[0] (User/Linear mapping) ===\n";

    // Read PGD[0] entry
    uint64_t pgdOffset = (SWAPPER_PGD - 0x40000000);
    if (pgdOffset + 8 <= memorySize) {
        uint64_t pgd0Entry = *(uint64_t*)((uint8_t*)memBase + pgdOffset);

        std::cout << "PGD[0] entry: 0x" << std::hex << pgd0Entry << std::dec << "\n";

        if (pgd0Entry & 1) {
            std::cout << "PGD[0] is valid\n";

            if (!(pgd0Entry & 2)) {
                std::cout << "PGD[0] is a huge page (unexpected!)\n";
            } else {
                uint64_t pudBase = pgd0Entry & 0x0000FFFFFFFFF000ULL;
                std::cout << "PGD[0] points to PUD at PA 0x"
                          << std::hex << pudBase << std::dec << "\n\n";

                if (pudBase >= 0x40000000 && pudBase < 0x40000000 + memorySize) {
                    WalkPUD(pudBase);
                }
            }
        } else {
            std::cout << "PGD[0] is NOT valid!\n";
        }
    }

    std::cout << "\n=== PGD[0] Mapping Statistics ===\n";
    std::cout << "Total pages mapped: " << pgd0_mapped_pages.size() << "\n";
    std::cout << "Total mapped memory: "
              << (pgd0_mapped_pages.size() * 4096 / 1024 / 1024) << " MB\n";
    std::cout << "1GB huge pages: " << huge_1gb_count << "\n";
    std::cout << "2MB huge pages: " << huge_2mb_count << "\n";
    std::cout << "4KB regular pages: " << regular_4kb_count << "\n";

    // Check our task_struct addresses
    std::cout << "\n=== Checking if task_structs are in PGD[0] mapping ===\n";
    uint64_t task_pas[] = {0x42238000, 0x4223c700, 0x42252380, 0x42254700};

    for (int i = 0; i < 4; i++) {
        uint64_t pa = task_pas[i];
        uint64_t page_pa = pa & ~0xFFF;

        if (pgd0_mapped_pages.count(page_pa)) {
            std::cout << "✓ PA 0x" << std::hex << pa
                      << " IS reachable through PGD[0]!\n";
        } else {
            std::cout << "✗ PA 0x" << std::hex << pa
                      << " is NOT in PGD[0] mapping\n";
        }
    }

    // Show what PA ranges PGD[0] DOES map
    if (!pgd0_mapped_pages.empty()) {
        std::cout << "\n=== PA ranges mapped by PGD[0] ===\n";
        uint64_t first = *pgd0_mapped_pages.begin();
        uint64_t last = *pgd0_mapped_pages.rbegin();
        std::cout << "Lowest PA:  0x" << std::hex << first << std::dec << "\n";
        std::cout << "Highest PA: 0x" << std::hex << last << std::dec << "\n";

        // Check for continuous 1GB regions
        std::cout << "\n1GB regions that include our task_struct area (0x422xxxxx):\n";
        for (uint64_t base = 0x40000000; base < 0x200000000; base += 0x40000000) {
            bool has_mapping = false;
            for (uint64_t p = base; p < base + 0x1000; p += 0x1000) {
                if (pgd0_mapped_pages.count(p)) {
                    has_mapping = true;
                    break;
                }
            }
            if (has_mapping) {
                std::cout << "  0x" << std::hex << base << " - 0x"
                          << (base + 0x40000000) << std::dec;
                if (base <= 0x42238000 && base + 0x40000000 > 0x42254700) {
                    std::cout << " <-- Contains our task_structs!";
                }
                std::cout << "\n";
            }
        }
    }

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}