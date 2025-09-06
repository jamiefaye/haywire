#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#define PAGE_SIZE 4096
#define BEACON_MAGIC 0x3142FACE
#define MAX_BEACONS 2048

// Simple timing helper
typedef struct {
    struct timeval start;
    struct timeval end;
    const char* name;
} Timer;

void timer_start(Timer* t, const char* name) {
    t->name = name;
    gettimeofday(&t->start, NULL);
}

double timer_end(Timer* t) {
    gettimeofday(&t->end, NULL);
    double elapsed = (t->end.tv_sec - t->start.tv_sec) * 1000.0;
    elapsed += (t->end.tv_usec - t->start.tv_usec) / 1000.0;
    return elapsed;
}

// Process info gathering
typedef struct {
    uint32_t pid;
    char name[64];
    uint64_t vsize_kb;
    uint64_t rss_kb;
} ProcessInfo;

int read_process_basic(uint32_t pid, ProcessInfo* info) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    
    char comm[256];
    unsigned long vsize, rss;
    
    fscanf(f, "%*d (%255[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %lu %lu",
           comm, &vsize, &rss);
    
    fclose(f);
    
    info->pid = pid;
    strncpy(info->name, comm, 63);
    info->vsize_kb = vsize / 1024;
    info->rss_kb = rss * (PAGE_SIZE / 1024);
    
    return 1;
}

// Section reading
typedef struct {
    uint64_t va_start;
    uint64_t va_end;
    uint32_t perms;
    char path[128];
} SectionInfo;

int read_process_sections(uint32_t pid, SectionInfo* sections, int max) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/maps", pid);
    
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    
    int count = 0;
    char line[512];
    
    while (fgets(line, sizeof(line), f) && count < max) {
        char perms[5];
        int n = sscanf(line, "%llx-%llx %4s %*x %*x:%*x %*u %127s",
                       &sections[count].va_start, 
                       &sections[count].va_end,
                       perms,
                       sections[count].path);
        
        if (n >= 3) {
            sections[count].perms = 0;
            if (perms[0] == 'r') sections[count].perms |= 1;
            if (perms[1] == 'w') sections[count].perms |= 2;
            if (perms[2] == 'x') sections[count].perms |= 4;
            if (n < 4) sections[count].path[0] = 0;
            count++;
        }
    }
    
    fclose(f);
    return count;
}

// Pagemap reading (needs root)
int read_process_pagemap(uint32_t pid, uint64_t va_start, uint64_t va_end, uint64_t* entries, int max) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/pagemap", pid);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    
    uint64_t start_page = va_start / PAGE_SIZE;
    uint64_t end_page = va_end / PAGE_SIZE;
    uint64_t page_count = end_page - start_page;
    
    if (page_count > max) page_count = max;
    
    lseek(fd, start_page * 8, SEEK_SET);
    int n = read(fd, entries, page_count * 8) / 8;
    
    close(fd);
    return n;
}

int main() {
    printf("=== Companion Benchmark ===\n");
    printf("Testing data collection performance...\n\n");
    
    Timer timer;
    
    // Benchmark 1: Count processes
    timer_start(&timer, "Count processes");
    DIR* proc = opendir("/proc");
    int process_count = 0;
    struct dirent* entry;
    
    while ((entry = readdir(proc)) != NULL) {
        if (atoi(entry->d_name) > 0) process_count++;
    }
    closedir(proc);
    double count_time = timer_end(&timer);
    printf("Count processes: %d processes in %.2f ms\n", process_count, count_time);
    
    // Benchmark 2: Read all process basic info
    timer_start(&timer, "Read process info");
    ProcessInfo* processes = malloc(process_count * sizeof(ProcessInfo));
    proc = opendir("/proc");
    int actual_count = 0;
    
    while ((entry = readdir(proc)) != NULL) {
        uint32_t pid = atoi(entry->d_name);
        if (pid > 0 && actual_count < process_count) {
            if (read_process_basic(pid, &processes[actual_count])) {
                actual_count++;
            }
        }
    }
    closedir(proc);
    double proc_time = timer_end(&timer);
    printf("Read process info: %d processes in %.2f ms (%.3f ms per process)\n", 
           actual_count, proc_time, proc_time / actual_count);
    
    // Benchmark 3: Read sections for all processes
    timer_start(&timer, "Read all sections");
    int total_sections = 0;
    SectionInfo sections[200];  // Temp buffer
    
    for (int i = 0; i < actual_count; i++) {
        int n = read_process_sections(processes[i].pid, sections, 200);
        total_sections += n;
    }
    double section_time = timer_end(&timer);
    printf("Read all sections: %d sections in %.2f ms (%.3f ms per process)\n",
           total_sections, section_time, section_time / actual_count);
    
    // Benchmark 4: Read sections for single process
    timer_start(&timer, "Single process sections");
    int n = read_process_sections(getpid(), sections, 200);
    double single_section = timer_end(&timer);
    printf("Single process sections: %d sections in %.2f ms\n", n, single_section);
    
    // Benchmark 5: Pagemap reading (will fail without root)
    timer_start(&timer, "Pagemap test");
    uint64_t pagemap_entries[1000];
    int pagemap_count = 0;
    
    if (n > 0) {
        // Try to read pagemap for first section
        pagemap_count = read_process_pagemap(getpid(), 
                                             sections[0].va_start,
                                             sections[0].va_end,
                                             pagemap_entries, 1000);
    }
    double pagemap_time = timer_end(&timer);
    
    if (pagemap_count > 0) {
        printf("Pagemap reading: %d pages in %.2f ms\n", pagemap_count, pagemap_time);
    } else {
        printf("Pagemap reading: Failed (need root?)\n");
    }
    
    // Benchmark 6: Full cycle time
    timer_start(&timer, "Full update cycle");
    
    // Simulate full companion update
    for (int i = 0; i < actual_count; i++) {
        read_process_basic(processes[i].pid, &processes[i]);
        int n = read_process_sections(processes[i].pid, sections, 200);
    }
    
    double full_cycle = timer_end(&timer);
    printf("\nFull update cycle: %.2f ms total\n", full_cycle);
    
    // Summary
    printf("\n=== Performance Summary ===\n");
    printf("Process discovery: %.2f ms\n", count_time);
    printf("Process info: %.2f ms (%.1f processes/ms)\n", proc_time, actual_count/proc_time);
    printf("Section reading: %.2f ms (%.1f processes/ms)\n", section_time, actual_count/section_time);
    printf("Full cycle: %.2f ms\n", full_cycle);
    printf("Theoretical max refresh rate: %.1f Hz\n", 1000.0/full_cycle);
    
    // Test focus mode - just 10 processes
    printf("\n=== Focus Mode Test (10 processes) ===\n");
    timer_start(&timer, "Focus mode");
    
    for (int i = 0; i < 10 && i < actual_count; i++) {
        read_process_basic(processes[i].pid, &processes[i]);
        read_process_sections(processes[i].pid, sections, 200);
    }
    
    double focus_time = timer_end(&timer);
    printf("10 process update: %.2f ms\n", focus_time);
    printf("Theoretical focus refresh rate: %.1f Hz\n", 1000.0/focus_time);
    
    free(processes);
    return 0;
}