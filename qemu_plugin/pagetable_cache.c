/*
 * Haywire QEMU Plugin - Page Table Cache
 * 
 * This plugin monitors page table walks and caches VA->PA translations
 * in shared memory for fast access by Haywire.
 *
 * Compile with:
 * gcc -shared -fPIC -O2 -o pagetable_cache.so pagetable_cache.c -lglib-2.0
 */

#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* Shared memory structure for VA->PA mappings */
#define MAX_ENTRIES (1024 * 1024)  // 1M entries
#define SHM_NAME "/haywire_pagetable"

typedef struct {
    uint64_t va;
    uint64_t pa;
    uint32_t pid;
    uint32_t valid;
} translation_entry_t;

typedef struct {
    uint32_t magic;  // 0x48415957 "HAYW"
    uint32_t version;
    uint64_t num_entries;
    uint64_t write_index;
    pthread_mutex_t lock;
    translation_entry_t entries[MAX_ENTRIES];
} shared_cache_t;

static shared_cache_t *cache = NULL;
static int shm_fd = -1;
static GHashTable *seen_translations = NULL;
static pthread_mutex_t local_lock = PTHREAD_MUTEX_INITIALIZER;

/* Track current process context (simplified - real impl would track CR3/TTBR) */
static __thread uint32_t current_pid = 0;

/* Initialize shared memory */
static int init_shared_memory(void)
{
    /* Create or open shared memory */
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return -1;
    }
    
    /* Set size */
    size_t shm_size = sizeof(shared_cache_t);
    if (ftruncate(shm_fd, shm_size) < 0) {
        perror("ftruncate");
        return -1;
    }
    
    /* Map it */
    cache = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (cache == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    
    /* Initialize if first time */
    if (cache->magic != 0x48415957) {
        memset(cache, 0, sizeof(*cache));
        cache->magic = 0x48415957;
        cache->version = 1;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&cache->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    
    fprintf(stderr, "Haywire plugin: Shared memory initialized at %s\n", SHM_NAME);
    return 0;
}

/* Add a translation to the cache */
static void add_translation(uint64_t va, uint64_t pa)
{
    if (!cache) return;
    
    /* Create key for deduplication */
    gchar *key = g_strdup_printf("%lx:%lx:%u", va, pa, current_pid);
    
    pthread_mutex_lock(&local_lock);
    
    /* Check if we've seen this translation recently */
    if (g_hash_table_contains(seen_translations, key)) {
        g_free(key);
        pthread_mutex_unlock(&local_lock);
        return;
    }
    
    /* Add to local tracking */
    g_hash_table_add(seen_translations, key);
    
    /* Add to shared cache */
    pthread_mutex_lock(&cache->lock);
    
    uint64_t idx = cache->write_index % MAX_ENTRIES;
    cache->entries[idx].va = va & ~0xFFFULL;  // Page align
    cache->entries[idx].pa = pa & ~0xFFFULL;
    cache->entries[idx].pid = current_pid;
    cache->entries[idx].valid = 1;
    
    cache->write_index++;
    if (cache->num_entries < MAX_ENTRIES) {
        cache->num_entries++;
    }
    
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_unlock(&local_lock);
}

/* Called on every memory access */
static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hwaddr = qemu_plugin_get_hwaddr(info, vaddr);
    if (hwaddr) {
        uint64_t paddr = qemu_plugin_hwaddr_phys_addr(hwaddr);
        
        /* Add to cache */
        add_translation(vaddr, paddr);
    }
}

/* Called when translating each basic block */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        
        /* Only track memory loads for now (not stores) to reduce overhead */
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_R, NULL);
    }
}

/* Plugin installation */
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    /* Parse arguments */
    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "pid=")) {
            current_pid = atoi(argv[i] + 4);
            fprintf(stderr, "Haywire plugin: Tracking PID %u\n", current_pid);
        }
    }
    
    /* Initialize shared memory */
    if (init_shared_memory() < 0) {
        return -1;
    }
    
    /* Initialize dedup table */
    seen_translations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    
    /* Register translation callback */
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    
    fprintf(stderr, "Haywire plugin: Loaded successfully\n");
    return 0;
}

/* Cleanup */
QEMU_PLUGIN_EXPORT void qemu_plugin_exit(qemu_plugin_id_t id, void *p)
{
    if (cache) {
        munmap(cache, sizeof(shared_cache_t));
    }
    if (shm_fd >= 0) {
        close(shm_fd);
    }
    if (seen_translations) {
        g_hash_table_destroy(seen_translations);
    }
}