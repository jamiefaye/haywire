/*
 * ARM64 Page Table Walker for QEMU Plugin
 * 
 * This walks ARM64 page tables directly in guest memory
 * instead of relying on QEMU's translation hooks.
 */

#include <inttypes.h>
#include <stdio.h>
#include <qemu-plugin.h>

/* ARM64 Page Table Format (4KB pages, 48-bit VA) */
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define TABLE_SHIFT     9
#define TABLE_SIZE      (1 << TABLE_SHIFT)  // 512 entries
#define TABLE_MASK      (TABLE_SIZE - 1)

/* Level shifts for 4KB pages */
#define L0_SHIFT        39  // Bits 47:39
#define L1_SHIFT        30  // Bits 38:30  
#define L2_SHIFT        21  // Bits 29:21
#define L3_SHIFT        12  // Bits 20:12

/* Descriptor types */
#define DESC_VALID      (1ULL << 0)
#define DESC_TABLE      (1ULL << 1)  // For L0-L2
#define DESC_PAGE       (1ULL << 1)  // For L3
#define DESC_AF         (1ULL << 10) // Access flag

/* Current TTBR values (would need to track per-CPU) */
static uint64_t current_ttbr0 = 0;
static uint64_t current_ttbr1 = 0;

/* Read 8 bytes from guest physical memory */
static uint64_t read_guest_phys(uint64_t paddr)
{
    /* PROBLEM: QEMU plugins don't have direct guest memory access!
     * We'd need to either:
     * 1. Use qemu_plugin_rw_memory() - but it needs a virtual address
     * 2. Patch QEMU to expose physical memory access
     * 3. Use a side channel (like /dev/mem in guest or memory backend)
     */
    
    // Placeholder - would need QEMU modification
    return 0;
}

/* Walk page tables for a VA */
static uint64_t walk_page_tables(uint64_t va, uint64_t ttbr)
{
    uint64_t table_base = ttbr & ~0xFFFULL;
    
    /* Level 0 lookup */
    uint64_t l0_index = (va >> L0_SHIFT) & TABLE_MASK;
    uint64_t l0_pte = read_guest_phys(table_base + l0_index * 8);
    
    if (!(l0_pte & DESC_VALID)) {
        return 0;  // Not mapped
    }
    
    /* Level 1 lookup */
    table_base = l0_pte & ~0xFFFULL;
    uint64_t l1_index = (va >> L1_SHIFT) & TABLE_MASK;
    uint64_t l1_pte = read_guest_phys(table_base + l1_index * 8);
    
    if (!(l1_pte & DESC_VALID)) {
        return 0;
    }
    
    /* Check for 1GB huge page */
    if (!(l1_pte & DESC_TABLE)) {
        return (l1_pte & ~0x3FFFFFFFULL) | (va & 0x3FFFFFFFULL);
    }
    
    /* Level 2 lookup */
    table_base = l1_pte & ~0xFFFULL;
    uint64_t l2_index = (va >> L2_SHIFT) & TABLE_MASK;
    uint64_t l2_pte = read_guest_phys(table_base + l2_index * 8);
    
    if (!(l2_pte & DESC_VALID)) {
        return 0;
    }
    
    /* Check for 2MB huge page */
    if (!(l2_pte & DESC_TABLE)) {
        return (l2_pte & ~0x1FFFFFULL) | (va & 0x1FFFFFULL);
    }
    
    /* Level 3 lookup */
    table_base = l2_pte & ~0xFFFULL;
    uint64_t l3_index = (va >> L3_SHIFT) & TABLE_MASK;
    uint64_t l3_pte = read_guest_phys(table_base + l3_index * 8);
    
    if (!(l3_pte & DESC_VALID)) {
        return 0;
    }
    
    /* 4KB page */
    return (l3_pte & ~0xFFFULL) | (va & 0xFFFULL);
}

/* Hook system register writes to catch TTBR changes */
static void vcpu_sysreg_write(unsigned int cpu_index, uint64_t addr,
                              uint64_t value, void *udata)
{
    /* TTBR0_EL1: Translation Table Base Register 0 */
    if (addr == 0xC002) {  // TTBR0_EL1 encoding
        current_ttbr0 = value;
        fprintf(stderr, "TTBR0 = 0x%016lx\n", value);
        
        /* Could dump entire page table here */
        // dump_all_mappings(value);
    }
    
    /* TTBR1_EL1: Translation Table Base Register 1 */
    if (addr == 0xC003) {  // TTBR1_EL1 encoding  
        current_ttbr1 = value;
        fprintf(stderr, "TTBR1 = 0x%016lx\n", value);
    }
}

/* The BIG PROBLEMS with this approach:
 * 
 * 1. QEMU plugins can't read guest physical memory directly
 *    - Need qemu_plugin_rw_memory() but it takes VA not PA
 *    - Would need to patch QEMU to add this API
 * 
 * 2. Can't easily hook system register writes
 *    - Would need to instrument specific instructions (MSR)
 *    - Or patch QEMU to expose these events
 * 
 * 3. Page table format varies by configuration
 *    - Different page sizes (4K, 16K, 64K)
 *    - Different VA sizes (39, 42, 48 bits)
 *    - Different granules and levels
 * 
 * 4. Need to handle all the complexities:
 *    - TLB invalidation
 *    - Access permissions
 *    - Memory attributes
 *    - ASID/VMID tagging
 */

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    fprintf(stderr, "Page walker plugin: Would need QEMU patches to work!\n");
    
    /* We'd need something like:
     * qemu_plugin_register_sysreg_cb(id, vcpu_sysreg_write);
     * 
     * But this API doesn't exist in stock QEMU!
     */
    
    return 0;
}