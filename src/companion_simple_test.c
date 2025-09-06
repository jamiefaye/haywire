#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096
#define BEACON_MAGIC 0x3142FACE

int main() {
    printf("=== Simple Beacon Test ===\n");
    
    // Allocate 1MB aligned memory
    size_t size = 1024 * 1024;
    void* mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    
    printf("Allocated 1MB at %p\n", mem);
    
    // Fill with beacon pattern
    uint32_t* data = (uint32_t*)mem;
    for (int i = 0; i < 256; i++) {
        data[i] = BEACON_MAGIC;
    }
    
    printf("Wrote beacon pattern (first 1KB filled with 0x%08X)\n", BEACON_MAGIC);
    printf("Virtual address: %p\n", mem);
    
    // Keep alive and periodically update
    int counter = 0;
    while (1) {
        data[256] = counter++;  // Update counter after pattern
        printf("Heartbeat %d - pattern at %p\n", counter, mem);
        sleep(5);
        
        if (counter > 20) break;  // Run for 100 seconds
    }
    
    munmap(mem, size);
    return 0;
}