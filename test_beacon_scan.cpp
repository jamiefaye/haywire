#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdint>
#include <map>

static constexpr uint32_t BEACON_MAGIC = 0x3142FACE;
static constexpr size_t PAGE_SIZE = 4096;

int main() {
    int fd = open("/tmp/haywire-vm-mem", O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open memory file\n";
        return 1;
    }
    
    struct stat st;
    fstat(fd, &st);
    size_t memSize = st.st_size;
    
    void* memBase = mmap(nullptr, memSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to map memory\n";
        close(fd);
        return 1;
    }
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    std::cout << "Scanning " << memSize / (1024*1024) << " MB of memory\n";
    
    // Count beacon pages by category
    std::map<uint32_t, int> categoryCounts;
    int totalBeacons = 0;
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        uint32_t* page = reinterpret_cast<uint32_t*>(mem + offset);
        
        if (page[0] == BEACON_MAGIC) {
            totalBeacons++;
            
            // Get category from offset 12
            uint32_t category = page[3];
            if (category < 10) {  // Sanity check
                categoryCounts[category]++;
                
                // Show first few of each category
                if (categoryCounts[category] <= 3) {
                    std::cout << "  Found cat " << category << " beacon at offset 0x" 
                              << std::hex << offset << std::dec 
                              << " (page " << offset/PAGE_SIZE << ")\n";
                }
            }
        }
    }
    
    std::cout << "\nTotal beacons found: " << totalBeacons << "\n";
    std::cout << "By category:\n";
    for (auto& [cat, count] : categoryCounts) {
        std::cout << "  Category " << cat << ": " << count << " pages\n";
    }
    
    munmap(memBase, memSize);
    close(fd);
    
    return 0;
}