#!/bin/bash
# Extract kernel structure offsets from running VM
# Requires VM to be booted with QGA (QEMU Guest Agent) socket

set -e

VM_MEM="/tmp/haywire-vm-mem"
QGA_SOCK="/tmp/qga.sock"

echo "=== Kernel Offset Extractor ==="
echo "Extracting offsets from VM without SSH..."

# Method 1: Try QGA socket (simplest if available)
if [ -S "$QGA_SOCK" ]; then
    echo "Using QEMU Guest Agent..."

    # Execute pahole inside VM via guest-exec
    cat << 'QGASCRIPT' | nc -U "$QGA_SOCK"
{"execute":"guest-exec", "arguments":{"path":"/usr/bin/pahole", "arg":["-C", "task_struct", "/sys/kernel/btf/vmlinux"], "capture-output":true}}
QGASCRIPT

    echo "Note: Parse QGA JSON response to get offsets"
    echo "Full automation would require jq and guest-exec-status"
else
    echo "QGA socket not found at $QGA_SOCK"
fi

# Method 2: Manual extraction template
cat << 'EOF'

=== MANUAL METHOD (if automated fails) ===

Boot VM and run these commands ONCE:

# Inside VM (via console, not SSH needed):
sudo apt-get install -y dwarves
pahole -C task_struct /sys/kernel/btf/vmlinux | grep -E 'pid |comm |mm |tasks ' > /tmp/task.txt
pahole -C mm_struct /sys/kernel/btf/vmlinux | grep -E 'pgd |mm_mt |mm_users ' > /tmp/mm.txt
pahole -C vm_area_struct /sys/kernel/btf/vmlinux | grep -E 'vm_start |vm_end |vm_flags |vm_file ' > /tmp/vma.txt
cat /tmp/task.txt /tmp/mm.txt /tmp/vma.txt

# Then copy-paste the output and run this parser:

EOF

cat << 'PARSER' > /tmp/parse_offsets.py
#!/usr/bin/env python3
"""Parse pahole output to generate C++ struct"""

import re
import sys

# Paste pahole output here or pipe it in
# Example: python3 parse_offsets.py < pahole_output.txt

offsets = {}
for line in sys.stdin:
    # Match lines like: "pid_t pid; /* 888 4 */"
    match = re.search(r'(\w+)\s+(\w+);.*?/\*\s+(\d+)\s+\d+\s+\*/', line)
    if match:
        field_name = match.group(2)
        offset = int(match.group(3))
        offsets[field_name] = offset

print("struct KernelOffsets {")
if 'pid' in offsets:
    print(f"    size_t pid = {offsets['pid']};")
if 'comm' in offsets:
    print(f"    size_t comm = {offsets['comm']};")
if 'mm' in offsets:
    print(f"    size_t mm = {offsets['mm']};")
if 'tasks' in offsets:
    print(f"    size_t tasks_list = {offsets['tasks']};")
    print(f"    size_t tasks_next = {offsets['tasks']};")
    print(f"    size_t tasks_prev = {offsets['tasks']} + 8;")
if 'pgd' in offsets:
    print(f"    size_t mm_pgd = {offsets['pgd']};")
if 'mm_mt' in offsets:
    print(f"    size_t mm_mt = {offsets['mm_mt']};")
if 'mm_users' in offsets:
    print(f"    size_t mm_users = {offsets['mm_users']};")
if 'vm_start' in offsets:
    print(f"    size_t vma_start = {offsets['vm_start']};")
if 'vm_end' in offsets:
    print(f"    size_t vma_end = {offsets['vm_end']};")
if 'vm_flags' in offsets:
    print(f"    size_t vma_flags = {offsets['vm_flags']};")
if 'vm_file' in offsets:
    print(f"    size_t vma_file = {offsets['vm_file']};")
print("};")
PARSER

chmod +x /tmp/parse_offsets.py

echo ""
echo "Parser created at: /tmp/parse_offsets.py"
echo "Run it after getting pahole output"
