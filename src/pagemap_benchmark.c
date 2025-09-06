#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

#define PAGE_SIZE 4096

typedef struct {
    struct timeval start;
    struct timeval end;
} Timer;

void timer_start(Timer* t) {
    gettimeofday(&t->start, NULL);
}

double timer_end_ms(Timer* t) {
    gettimeofday(&t->end, NULL);
    double elapsed = (t->end.tv_sec - t->start.tv_sec) * 1000.0;
    elapsed += (t->end.tv_usec - t->start.tv_usec) / 1000.0;
    return elapsed;
}

int main() {
    printf("=== Pagemap Transfer Benchmark ===\n\n");
    
    Timer timer;
    
    // First, read our own maps to get VA ranges
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        perror("Cannot open /proc/self/maps");
        return 1;
    }
    
    // Find a few different sized regions
    char line[512];
    uint64_t small_start = 0, small_end = 0;
    uint64_t medium_start = 0, medium_end = 0;
    uint64_t large_start = 0, large_end = 0;
    uint64_t heap_start = 0, heap_end = 0;
    
    while (fgets(line, sizeof(line), maps)) {
        uint64_t start, end;
        char perms[5], path[256] = "";
        
        sscanf(line, "%lx-%lx %4s %*x %*x:%*x %*u %255s",
               &start, &end, perms, path);
        
        uint64_t size = end - start;
        uint64_t pages = size / PAGE_SIZE;
        
        if (strstr(path, "[heap]")) {
            heap_start = start;
            heap_end = end;
        } else if (pages < 10 && !small_start) {
            small_start = start;
            small_end = end;
        } else if (pages < 100 && !medium_start) {
            medium_start = start;
            medium_end = end;
        } else if (pages < 1000 && !large_start) {
            large_start = start;
            large_end = end;
        }
    }
    fclose(maps);
    
    // Open pagemap
    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        printf("Cannot open /proc/self/pagemap (need root?)\n");
        printf("Try: sudo ./pagemap_benchmark\n");
        return 1;
    }
    
    // Test different region sizes
    struct {
        const char* name;
        uint64_t va_start;
        uint64_t va_end;
    } test_regions[] = {
        {"Small region", small_start, small_end},
        {"Medium region", medium_start, medium_end},
        {"Large region", large_start, large_end},
        {"Heap", heap_start, heap_end},
    };
    
    printf("Region sizes:\n");
    for (int i = 0; i < 4; i++) {
        if (test_regions[i].va_start) {
            uint64_t pages = (test_regions[i].va_end - test_regions[i].va_start) / PAGE_SIZE;
            printf("  %s: %lu pages (%lu KB)\n", 
                   test_regions[i].name, pages, pages * 4);
        }
    }
    printf("\n");
    
    // Benchmark reading different sized regions
    for (int i = 0; i < 4; i++) {
        if (!test_regions[i].va_start) continue;
        
        uint64_t start = test_regions[i].va_start;
        uint64_t end = test_regions[i].va_end;
        uint64_t pages = (end - start) / PAGE_SIZE;
        
        // Allocate buffer for pagemap entries
        uint64_t* entries = malloc(pages * sizeof(uint64_t));
        
        timer_start(&timer);
        
        // Seek to start
        off_t offset = (start / PAGE_SIZE) * sizeof(uint64_t);
        lseek(pagemap_fd, offset, SEEK_SET);
        
        // Read all entries
        ssize_t bytes_read = read(pagemap_fd, entries, pages * sizeof(uint64_t));
        
        double read_time = timer_end_ms(&timer);
        
        if (bytes_read > 0) {
            uint64_t entries_read = bytes_read / sizeof(uint64_t);
            
            // Count present pages
            int present = 0;
            for (int j = 0; j < entries_read; j++) {
                if (entries[j] & (1ULL << 63)) present++;
            }
            
            printf("%s:\n", test_regions[i].name);
            printf("  Read %lu entries in %.3f ms\n", entries_read, read_time);
            printf("  %.3f μs per page\n", (read_time * 1000) / entries_read);
            printf("  %d/%lu pages present\n", present, entries_read);
            printf("  Transfer size: %lu KB\n", (entries_read * 8) / 1024);
            printf("  Throughput: %.1f MB/s\n\n", 
                   (entries_read * 8.0 / 1024 / 1024) / (read_time / 1000));
        }
        
        free(entries);
    }
    
    // Test full process pagemap read
    printf("=== Full Process Pagemap ===\n");
    
    // Count total mapped pages
    maps = fopen("/proc/self/maps", "r");
    uint64_t total_pages = 0;
    while (fgets(line, sizeof(line), maps)) {
        uint64_t start, end;
        sscanf(line, "%lx-%lx", &start, &end);
        total_pages += (end - start) / PAGE_SIZE;
    }
    fclose(maps);
    
    printf("Total mapped: %lu pages (%lu MB)\n", total_pages, (total_pages * 4) / 1024);
    
    // Estimate time for different process sizes
    double us_per_page = 0.1;  // Rough estimate from above
    
    printf("\nEstimated pagemap transfer times:\n");
    printf("  Small process (100 pages):    %.2f ms\n", (100 * us_per_page) / 1000);
    printf("  Medium process (1000 pages):  %.2f ms\n", (1000 * us_per_page) / 1000);
    printf("  Large process (10000 pages):  %.2f ms\n", (10000 * us_per_page) / 1000);
    printf("  Chrome-sized (100000 pages):  %.2f ms\n", (100000 * us_per_page) / 1000);
    
    close(pagemap_fd);
    return 0;
}