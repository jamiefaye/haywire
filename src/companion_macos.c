#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define PAGE_SIZE 4096
#define BEACON_MAGIC 0x3142FACE
#define MAX_BEACONS 2048

// Beacon structures (same as Linux version)
typedef struct {
    uint32_t magic;
    uint32_t session_id;
    uint32_t beacon_type;
    uint32_t type_index;
    uint8_t data[4080];
} __attribute__((packed)) BeaconPage;

// Process entry - macOS version
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint64_t vsize_kb;
    uint64_t rss_kb;
    uint64_t cpu_time;
    char name[64];
    char state;
    uint8_t padding[3];
} __attribute__((packed)) ProcessEntry;

// Memory section entry - macOS version
typedef struct {
    uint32_t pid;
    uint64_t va_start;
    uint64_t va_end;
    uint32_t perms;
    uint32_t offset;
    uint32_t major;
    uint32_t minor;
    uint32_t inode;
    char path[128];
} __attribute__((packed)) SectionEntry;

static BeaconPage* beacon_array = NULL;
static uint32_t session_id = 0;
static volatile int running = 1;
static uint32_t next_beacon = 0;

// Get process list using sysctl
int get_process_list(ProcessEntry* entries, int max_entries) {
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t size;
    
    // Get size needed
    if (sysctl(mib, 4, NULL, &size, NULL, 0) < 0) {
        return 0;
    }
    
    struct kinfo_proc* procs = malloc(size);
    if (!procs) return 0;
    
    // Get actual process list
    if (sysctl(mib, 4, procs, &size, NULL, 0) < 0) {
        free(procs);
        return 0;
    }
    
    int count = size / sizeof(struct kinfo_proc);
    int actual = 0;
    
    for (int i = 0; i < count && actual < max_entries; i++) {
        struct kinfo_proc* kp = &procs[i];
        
        // Skip kernel_task and dead processes
        if (kp->kp_proc.p_pid == 0) continue;
        
        ProcessEntry* e = &entries[actual];
        e->pid = kp->kp_proc.p_pid;
        e->ppid = kp->kp_eproc.e_ppid;
        e->uid = kp->kp_eproc.e_ucred.cr_uid;
        e->gid = kp->kp_eproc.e_ucred.cr_gid;
        
        // Get more detailed info using proc_pidinfo
        struct proc_taskinfo ti;
        if (proc_pidinfo(e->pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) > 0) {
            e->vsize_kb = ti.pti_virtual_size / 1024;
            e->rss_kb = ti.pti_resident_size / 1024;
            e->cpu_time = ti.pti_total_user + ti.pti_total_system;
        }
        
        // Get process name
        proc_name(e->pid, e->name, sizeof(e->name));
        
        // Simple state mapping
        if (kp->kp_proc.p_stat == SRUN) e->state = 'R';
        else if (kp->kp_proc.p_stat == SSLEEP) e->state = 'S';
        else if (kp->kp_proc.p_stat == SSTOP) e->state = 'T';
        else if (kp->kp_proc.p_stat == SZOMB) e->state = 'Z';
        else e->state = '?';
        
        actual++;
    }
    
    free(procs);
    return actual;
}

// Get memory regions using mach_vm_region
int get_memory_regions(uint32_t pid, SectionEntry* sections, int max_sections) {
    mach_port_t task;
    kern_return_t kr;
    
    // Get task port for process
    kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        // Need root or entitlements for other processes
        return 0;
    }
    
    int count = 0;
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t info_count;
    mach_port_t object_name;
    
    while (count < max_sections) {
        info_count = VM_REGION_BASIC_INFO_COUNT_64;
        kr = mach_vm_region(task, &addr, &size, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &info_count, &object_name);
        
        if (kr != KERN_SUCCESS) break;
        
        SectionEntry* s = &sections[count];
        s->pid = pid;
        s->va_start = addr;
        s->va_end = addr + size;
        s->perms = 0;
        
        if (info.protection & VM_PROT_READ) s->perms |= 1;
        if (info.protection & VM_PROT_WRITE) s->perms |= 2;
        if (info.protection & VM_PROT_EXECUTE) s->perms |= 4;
        if (info.shared) s->perms |= 8;
        
        // macOS doesn't provide file paths easily through mach_vm_region
        s->path[0] = 0;
        s->offset = info.offset;
        s->major = 0;
        s->minor = 0; 
        s->inode = 0;
        
        count++;
        addr += size;
    }
    
    mach_port_deallocate(mach_task_self(), task);
    return count;
}

// Allocate beacon pages in a way that's findable
void* allocate_findable_memory() {
    // Try to allocate at a specific address that's easy to find
    // In QEMU guest, this should be in guest physical memory
    
    size_t total_size = MAX_BEACONS * PAGE_SIZE;
    
    // First try: Let system choose but align to large boundary
    void* mem = mmap(NULL, total_size, 
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (mem == MAP_FAILED) {
        return NULL;
    }
    
    // Write magic pattern at start for easier discovery
    uint32_t* magic_array = (uint32_t*)mem;
    for (int i = 0; i < 16; i++) {
        magic_array[i] = BEACON_MAGIC;
    }
    
    printf("Beacons allocated at %p (guest virtual)\n", mem);
    printf("In QEMU physical memory, look for repeated 0x%08X\n", BEACON_MAGIC);
    
    return mem;
}

void sighandler(int sig) {
    printf("\nShutting down macOS companion...\n");
    running = 0;
}

int main(int argc, char* argv[]) {
    printf("=== Haywire Companion for macOS ===\n");
    
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    // Allocate beacon memory
    beacon_array = (BeaconPage*)allocate_findable_memory();
    if (!beacon_array) {
        fprintf(stderr, "Failed to allocate beacon memory\n");
        return 1;
    }
    
    session_id = getpid();
    printf("Session ID: 0x%08X\n", session_id);
    
    // Main loop
    while (running) {
        // Reset beacon allocation
        next_beacon = 0;
        
        // Create control beacon
        beacon_array[0].magic = BEACON_MAGIC;
        beacon_array[0].session_id = session_id;
        beacon_array[0].beacon_type = 1; // CONTROL
        beacon_array[0].type_index = 0;
        
        // Get process list
        ProcessEntry processes[1000];
        int proc_count = get_process_list(processes, 1000);
        printf("Found %d processes\n", proc_count);
        
        // Pack into process beacons
        int proc_per_beacon = 40;
        int beacon_idx = 1;
        
        for (int i = 0; i < proc_count; i += proc_per_beacon) {
            beacon_array[beacon_idx].magic = BEACON_MAGIC;
            beacon_array[beacon_idx].session_id = session_id;
            beacon_array[beacon_idx].beacon_type = 2; // PROCLIST
            beacon_array[beacon_idx].type_index = beacon_idx;
            
            int count = (proc_count - i < proc_per_beacon) ? 
                       proc_count - i : proc_per_beacon;
            
            memcpy(beacon_array[beacon_idx].data + 12, 
                   &processes[i], count * sizeof(ProcessEntry));
            
            // Write header
            *(uint32_t*)(beacon_array[beacon_idx].data) = count;
            *(uint32_t*)(beacon_array[beacon_idx].data + 4) = proc_count;
            *(uint32_t*)(beacon_array[beacon_idx].data + 8) = 0xFFFFFFFF;
            
            beacon_idx++;
        }
        
        printf("Using %d beacons\n", beacon_idx);
        
        // Note: Memory regions would need root/entitlements
        // Skip for now unless running as root
        
        sleep(1);
    }
    
    // Clear beacons on exit
    memset(beacon_array, 0, MAX_BEACONS * PAGE_SIZE);
    munmap(beacon_array, MAX_BEACONS * PAGE_SIZE);
    
    return 0;
}