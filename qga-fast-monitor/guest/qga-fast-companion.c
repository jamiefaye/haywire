// Minimal proof-of-concept: Just share process count
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <time.h>
#include "../common/protocol.h"

#define SHM_PATH "/dev/shm/vm-monitor"
#define SHM_SIZE (32 * 1024 * 1024) // 32MB

static int count_processes() {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return -1;
    
    int count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(proc_dir)) != NULL) {
        // Check if directory name is a PID (all digits)
        char *endptr;
        strtol(entry->d_name, &endptr, 10);
        if (*endptr == '\0') {
            count++;
        }
    }
    
    closedir(proc_dir);
    return count;
}

int main() {
    // Create shared memory
    int fd = shm_open("vm-monitor", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return 1;
    }
    
    if (ftruncate(fd, SHM_SIZE) < 0) {
        perror("ftruncate");
        return 1;
    }
    
    // Map shared memory
    void *shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    
    struct shm_header *header = (struct shm_header *)shm;
    
    // Initialize header
    header->magic = SHM_MAGIC;
    header->version = SHM_VERSION;
    header->update_counter = 0;
    
    printf("QGA Fast Companion started - updating /dev/shm/vm-monitor\n");
    
    // Main loop - update every 100ms
    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        
        header->timestamp_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        header->num_processes = count_processes();
        header->update_counter++;
        
        // Memory barrier to ensure writes are visible
        __sync_synchronize();
        
        usleep(100000); // 100ms
    }
    
    return 0;
}