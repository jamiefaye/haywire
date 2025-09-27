#!/bin/bash
# Get kernel structure offsets using pahole
# Run this on the target VM or with the kernel debug symbols

echo "=== Kernel Structure Offsets for Haywire ==="
echo ""
echo "Note: Run this script on the VM with: ssh vm 'bash -s' < get_kernel_offsets.sh"
echo ""

# Check if pahole is available
if ! command -v pahole &> /dev/null; then
    echo "ERROR: pahole not found. Install with: sudo apt install dwarves"
    exit 1
fi

# Find kernel with debug symbols
KERNEL_DEBUG=""
if [ -f /usr/lib/debug/boot/vmlinux-$(uname -r) ]; then
    KERNEL_DEBUG="/usr/lib/debug/boot/vmlinux-$(uname -r)"
elif [ -f /boot/vmlinux-$(uname -r) ]; then
    KERNEL_DEBUG="/boot/vmlinux-$(uname -r)"
else
    echo "WARNING: No debug kernel found. Trying /proc/kcore (may have limited info)"
    KERNEL_DEBUG="/proc/kcore"
fi

echo "Using kernel: $KERNEL_DEBUG"
echo "Kernel version: $(uname -r)"
echo ""

# task_struct offsets
echo "=== task_struct offsets ==="
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "pid;|comm\[|mm;|files;|tasks\{" | head -20

echo ""
echo "=== Extracted task_struct offsets ==="
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "pid;" | sed 's/.*\/\* *\([0-9]*\).*/task.pid: 0x\1/'
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "comm\[" | sed 's/.*\/\* *\([0-9]*\).*/task.comm: 0x\1/'
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct mm_struct.*\*mm;" | sed 's/.*\/\* *\([0-9]*\).*/task.mm: 0x\1/'
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct files_struct.*\*files;" | sed 's/.*\/\* *\([0-9]*\).*/task.files: 0x\1/'
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct list_head.*tasks;" | sed 's/.*\/\* *\([0-9]*\).*/task.tasks: 0x\1/'

echo ""
echo "=== files_struct offsets ==="
pahole -C files_struct $KERNEL_DEBUG 2>/dev/null | grep -E "fdt;|fdtab;" | head -5

echo ""
echo "=== Extracted files_struct offsets ==="
pahole -C files_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct fdtable.*\*fdt;" | sed 's/.*\/\* *\([0-9]*\).*/files.fdt: 0x\1/'

echo ""
echo "=== fdtable offsets ==="
pahole -C fdtable $KERNEL_DEBUG 2>/dev/null | grep -E "max_fds;|fd;" | head -5

echo ""
echo "=== Extracted fdtable offsets ==="
pahole -C fdtable $KERNEL_DEBUG 2>/dev/null | grep -E "unsigned int.*max_fds;" | sed 's/.*\/\* *\([0-9]*\).*/fdt.max_fds: 0x\1/'
pahole -C fdtable $KERNEL_DEBUG 2>/dev/null | grep -E "struct file.*\*\*fd;" | sed 's/.*\/\* *\([0-9]*\).*/fdt.fd: 0x\1/'

echo ""
echo "=== file offsets ==="
pahole -C file $KERNEL_DEBUG 2>/dev/null | grep -E "f_inode;|f_path;" | head -5

echo ""
echo "=== Extracted file offsets ==="
pahole -C file $KERNEL_DEBUG 2>/dev/null | grep -E "struct inode.*\*f_inode;" | sed 's/.*\/\* *\([0-9]*\).*/file.inode: 0x\1/'
pahole -C file $KERNEL_DEBUG 2>/dev/null | grep -E "struct path.*f_path;" | sed 's/.*\/\* *\([0-9]*\).*/file.path: 0x\1/'

echo ""
echo "=== inode offsets ==="
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "i_ino;|i_mode;|i_size;" | head -5

echo ""
echo "=== Extracted inode offsets ==="
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "unsigned long.*i_ino;" | sed 's/.*\/\* *\([0-9]*\).*/inode.ino: 0x\1/'
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "umode_t.*i_mode;" | sed 's/.*\/\* *\([0-9]*\).*/inode.mode: 0x\1/'
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "loff_t.*i_size;" | sed 's/.*\/\* *\([0-9]*\).*/inode.size: 0x\1/'

echo ""
echo "=== mm_struct offsets (for reference) ==="
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "pgd;|mm_mt;|mm_users;|mm_count;" | head -10

echo ""
echo "=== Extracted mm_struct offsets ==="
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "pgd_t.*\*pgd;" | sed 's/.*\/\* *\([0-9]*\).*/mm.pgd: 0x\1/'
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct maple_tree.*mm_mt;" | sed 's/.*\/\* *\([0-9]*\).*/mm.mm_mt: 0x\1/'
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "atomic_t.*mm_users;" | sed 's/.*\/\* *\([0-9]*\).*/mm.mm_users: 0x\1/'

echo ""
echo "=== Summary for kernel-mem.ts ==="
echo "export const OFFSETS = {"
echo "    // task_struct"
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "pid;" | sed "s/.*\/\* *\([0-9]*\).*/    'task.pid': 0x\1,/" | sed 's/0x\([0-9]*\)/0x\1/'
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "comm\[" | sed "s/.*\/\* *\([0-9]*\).*/    'task.comm': 0x\1,/"
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct mm_struct.*\*mm;" | sed "s/.*\/\* *\([0-9]*\).*/    'task.mm': 0x\1,/"
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct files_struct.*\*files;" | sed "s/.*\/\* *\([0-9]*\).*/    'task.files': 0x\1,/"
pahole -C task_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct list_head.*tasks;" | sed "s/.*\/\* *\([0-9]*\).*/    'task.tasks': 0x\1,/"
echo ""
echo "    // files_struct"
pahole -C files_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct fdtable.*\*fdt;" | sed "s/.*\/\* *\([0-9]*\).*/    'files.fdt': 0x\1,/"
echo ""
echo "    // fdtable"
pahole -C fdtable $KERNEL_DEBUG 2>/dev/null | grep -E "unsigned int.*max_fds;" | sed "s/.*\/\* *\([0-9]*\).*/    'fdt.max_fds': 0x\1,/"
pahole -C fdtable $KERNEL_DEBUG 2>/dev/null | grep -E "struct file.*\*\*fd;" | sed "s/.*\/\* *\([0-9]*\).*/    'fdt.fd': 0x\1,/"
echo ""
echo "    // file"
pahole -C file $KERNEL_DEBUG 2>/dev/null | grep -E "struct inode.*\*f_inode;" | sed "s/.*\/\* *\([0-9]*\).*/    'file.inode': 0x\1,/"
echo ""
echo "    // inode"
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "unsigned long.*i_ino;" | sed "s/.*\/\* *\([0-9]*\).*/    'inode.ino': 0x\1,/"
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "umode_t.*i_mode;" | sed "s/.*\/\* *\([0-9]*\).*/    'inode.mode': 0x\1,/"
pahole -C inode $KERNEL_DEBUG 2>/dev/null | grep -E "loff_t.*i_size;" | sed "s/.*\/\* *\([0-9]*\).*/    'inode.size': 0x\1,/"
echo ""
echo "    // mm_struct"
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "pgd_t.*\*pgd;" | sed "s/.*\/\* *\([0-9]*\).*/    'mm.pgd': 0x\1,/"
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "struct maple_tree.*mm_mt;" | sed "s/.*\/\* *\([0-9]*\).*/    'mm.mm_mt': 0x\1,/"
pahole -C mm_struct $KERNEL_DEBUG 2>/dev/null | grep -E "atomic_t.*mm_users;" | sed "s/.*\/\* *\([0-9]*\).*/    'mm.mm_users': 0x\1/"
echo "};"