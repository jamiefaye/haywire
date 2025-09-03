#ifndef QGA_FAST_PROTOCOL_H
#define QGA_FAST_PROTOCOL_H

#include <stdint.h>

#define SHM_MAGIC 0xDEADBEEF
#define SHM_VERSION 1
#define MAX_COMM_LEN 16

struct shm_header {
    uint32_t magic;           // 0xDEADBEEF for validation
    uint32_t version;         // Protocol version
    uint64_t update_counter;  // Increments on each update
    uint64_t timestamp_ns;    // Clock monotonic timestamp
    uint32_t num_processes;
    uint32_t process_offset;  // Offset to process array
    uint32_t num_vmas;        // Total VMAs across all processes  
    uint32_t vma_offset;      // Offset to VMA array
};

struct process_entry {
    uint32_t pid;
    uint32_t tgid;
    uint64_t task_struct_addr;
    uint64_t mm_struct_addr;
    char comm[MAX_COMM_LEN];
    uint32_t num_vmas;        // VMAs for this process
    uint32_t vma_index;       // Index into VMA array
};

struct vma_entry {
    uint64_t start;
    uint64_t end;
    uint64_t flags;
    uint32_t pid;            // Which process owns this
};

#endif // QGA_FAST_PROTOCOL_H