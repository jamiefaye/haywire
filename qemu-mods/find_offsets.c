/* 
 * Compile in VM: gcc -o find_offsets find_offsets.c
 * Run: ./find_offsets
 * This will print the exact offsets for your kernel version
 */

#include <stdio.h>
#include <stddef.h>

// Dummy structures with common fields
// The actual kernel headers would have the real definitions
// but we can approximate based on common layouts

struct list_head {
    struct list_head *next, *prev;
};

struct mm_struct {
    char padding[0x48];  // Varies by kernel
    void *pgd;          // Page global directory
};

// Simplified task_struct - offsets vary by kernel config
struct task_struct {
    char padding1[0x398];     // Padding to pid
    int pid;                  // Process ID
    char padding2[0x150];     // Padding to tasks
    struct list_head tasks;   // Task list
    char padding3[0xE0];      // Padding to comm  
    char comm[16];            // Process name
    char padding4[0x100];     // More padding
    struct mm_struct *mm;     // Memory descriptor
};

int main() {
    printf("Approximate task_struct offsets:\n");
    printf("  tasks: 0x%lx\n", offsetof(struct task_struct, tasks));
    printf("  pid:   0x%lx\n", offsetof(struct task_struct, pid));
    printf("  comm:  0x%lx\n", offsetof(struct task_struct, comm));
    printf("  mm:    0x%lx\n", offsetof(struct task_struct, mm));
    printf("\n");
    printf("mm_struct offsets:\n");
    printf("  pgd:   0x%lx\n", offsetof(struct mm_struct, pgd));
    printf("\n");
    printf("Note: These are estimates. For exact values, you need:\n");
    printf("  1. Kernel headers: /usr/src/linux-headers-$(uname -r)\n");
    printf("  2. Or debug symbols: linux-image-$(uname -r)-dbgsym\n");
    return 0;
}