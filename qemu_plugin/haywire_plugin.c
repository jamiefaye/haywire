/*
 * Haywire QEMU Plugin for fast VA->PA translation
 * 
 * Compile with:
 * gcc -shared -fPIC -o haywire_plugin.so haywire_plugin.c
 */

#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static FILE *logfile;

/* Called on every memory access */
static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    /* We can see VA here, and QEMU knows the PA internally */
    struct qemu_plugin_hwaddr *hwaddr = qemu_plugin_get_hwaddr(info, vaddr);
    if (hwaddr) {
        uint64_t paddr = qemu_plugin_hwaddr_phys_addr(hwaddr);
        
        /* Log VA->PA mapping for analysis */
        if (logfile) {
            fprintf(logfile, "CPU%d: VA 0x%016lx -> PA 0x%016lx\n", 
                    cpu_index, vaddr, paddr);
            fflush(logfile);
        }
    }
}

/* Called on instruction execution */
static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    /* Could track instruction addresses here */
}

/* Called when translating each basic block */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        
        /* Register callback for memory operations */
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);
        
        /* Optionally track instruction execution */
        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                               QEMU_PLUGIN_CB_NO_REGS, NULL);
    }
}

/* Plugin installation */
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    /* Open log file */
    const char *logpath = "/tmp/haywire_va_pa.log";
    if (argc > 0) {
        logpath = argv[0];
    }
    
    logfile = fopen(logpath, "w");
    if (!logfile) {
        fprintf(stderr, "Haywire plugin: Cannot open log file %s\n", logpath);
        return -1;
    }
    
    fprintf(stderr, "Haywire plugin loaded, logging to %s\n", logpath);
    
    /* Register translation callback */
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    
    return 0;
}