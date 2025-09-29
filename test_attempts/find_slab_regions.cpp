#include <iostream>
#include <iomanip>
#include <map>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

void* memBase = nullptr;
size_t memorySize = 0;

// Known task_struct characteristics
const size_t TASK_STRUCT_SIZE = 9408;  // Approximate on ARM64
const size_t PID_OFFSET = 0x750;
const size_t COMM_OFFSET = 0x970;

// Track regions with valid task_structs
std::map<uint64_t, int> task_struct_pages;

bool LooksLikeTaskStruct(uint64_t offset) {
    if (offset + TASK_STRUCT_SIZE > memorySize) return false;

    uint8_t* ptr = (uint8_t*)memBase + offset;

    // Check PID
    uint32_t pid = *(uint32_t*)(ptr + PID_OFFSET);
    if (pid == 0 || pid > 100000) return false;

    // Check comm (process name)
    char* comm = (char*)(ptr + COMM_OFFSET);
    bool hasName = false;
    for (int i = 0; i < 16; i++) {
        if (comm[i] == 0) break;
        if (comm[i] >= 32 && comm[i] < 127) {
            hasName = true;
        } else if (comm[i] != 0) {
            return false;  // Non-printable
        }
    }

    return hasName;
}

void AnalyzeSlabPatterns() {
    std::cout << "=== Analyzing Memory for SLAB Patterns ===\n\n";

    // First pass: Find pages with task_structs
    std::cout << "Scanning for task_struct clusters...\n";

    for (uint64_t offset = 0; offset < memorySize; offset += 0x1000) {  // Page by page
        int found_in_page = 0;

        // Check multiple offsets within the page for task_structs
        for (uint64_t in_page = 0; in_page < 0x1000; in_page += 64) {  // 64-byte aligned
            if (LooksLikeTaskStruct(offset + in_page)) {
                found_in_page++;
            }
        }

        if (found_in_page > 0) {
            uint64_t pa = offset + 0x40000000;
            task_struct_pages[pa] = found_in_page;
        }

        if (offset % (100 * 1024 * 1024) == 0) {
            std::cout << "  Scanned " << (offset / 1024 / 1024) << " MB...\r" << std::flush;
        }
    }

    std::cout << "\n\nFound task_structs in " << task_struct_pages.size() << " pages\n\n";

    // Analyze clustering to find SLAB regions
    std::cout << "=== SLAB Region Analysis ===\n";

    if (!task_struct_pages.empty()) {
        uint64_t region_start = task_struct_pages.begin()->first;
        uint64_t region_end = region_start;
        int region_count = 0;

        for (auto& [pa, count] : task_struct_pages) {
            if (pa <= region_end + 0x10000) {  // Within 64KB
                region_end = pa;
                region_count += count;
            } else {
                // Gap too large, different region
                if (region_count > 5) {  // Significant region
                    std::cout << "SLAB Region: 0x" << std::hex << region_start
                              << " - 0x" << region_end
                              << " (" << std::dec << (region_end - region_start) / 1024 << " KB)"
                              << " with " << region_count << " task_structs\n";
                }
                region_start = pa;
                region_end = pa;
                region_count = count;
            }
        }

        // Don't forget last region
        if (region_count > 5) {
            std::cout << "SLAB Region: 0x" << std::hex << region_start
                      << " - 0x" << region_end
                      << " (" << std::dec << (region_end - region_start) / 1024 << " KB)"
                      << " with " << region_count << " task_structs\n";
        }
    }

    // Analyze spacing to detect SLAB allocation pattern
    std::cout << "\n=== SLAB Allocation Pattern ===\n";

    std::map<int, int> spacing_histogram;
    uint64_t prev_pa = 0;

    for (auto& [pa, count] : task_struct_pages) {
        if (prev_pa != 0 && pa - prev_pa < 0x100000) {  // Within 1MB
            int spacing = (pa - prev_pa) / 0x1000;  // In pages
            spacing_histogram[spacing]++;
        }
        prev_pa = pa;
    }

    std::cout << "Page spacing between task_struct pages:\n";
    for (auto& [spacing, count] : spacing_histogram) {
        if (count > 2) {  // Repeated pattern
            std::cout << "  " << spacing << " pages apart: " << count << " occurrences";
            if (spacing == 1) std::cout << " (CONTIGUOUS SLAB!)";
            std::cout << "\n";
        }
    }

    // Find densest region (likely main SLAB cache)
    std::cout << "\n=== Primary SLAB Cache Location ===\n";

    uint64_t max_density_start = 0;
    int max_density_count = 0;

    for (auto& [pa, count] : task_struct_pages) {
        // Count task_structs within 256KB window
        int window_count = 0;
        for (auto& [other_pa, other_count] : task_struct_pages) {
            if (other_pa >= pa && other_pa < pa + 0x40000) {  // 256KB window
                window_count += other_count;
            }
        }

        if (window_count > max_density_count) {
            max_density_count = window_count;
            max_density_start = pa;
        }
    }

    if (max_density_start != 0) {
        std::cout << "Highest density region (likely main task_struct SLAB):\n";
        std::cout << "  Starting at PA: 0x" << std::hex << max_density_start << std::dec << "\n";
        std::cout << "  Contains " << max_density_count << " task_structs in 256KB window\n";
        std::cout << "\nRECOMMENDATION: Focus search on PA range 0x"
                  << std::hex << (max_density_start & ~0xFFFFF)
                  << " - 0x" << ((max_density_start & ~0xFFFFF) + 0x100000) << std::dec << "\n";
        std::cout << "This 1MB region likely contains most active task_structs\n";
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

    AnalyzeSlabPatterns();

    munmap(memBase, memorySize);
    close(fd);
    return 0;
}