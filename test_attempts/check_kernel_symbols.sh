#!/bin/bash

echo "=== Checking what kernel symbols are exposed ==="
echo ""

echo "1. /proc/kallsyms (live kernel symbols):"
echo "   Usually enabled even in production for debugging"
if [ -r /proc/kallsyms ]; then
    echo "   ✓ Available"
    echo "   Sample entries:"
    grep -E "init_task|init_mm|swapper" /proc/kallsyms | head -5
else
    echo "   ✗ Not readable (need root or kptr_restrict=0)"
fi

echo ""
echo "2. /boot/System.map-* (symbol table):"
ls -la /boot/System.map-* 2>/dev/null || echo "   ✗ Not found"

echo ""
echo "3. Kernel pointer restriction (/proc/sys/kernel/kptr_restrict):"
if [ -r /proc/sys/kernel/kptr_restrict ]; then
    value=$(cat /proc/sys/kernel/kptr_restrict)
    case $value in
        0) echo "   0 = Addresses exposed (no restriction)" ;;
        1) echo "   1 = Addresses hidden from non-root (default)" ;;
        2) echo "   2 = Addresses hidden from everyone" ;;
    esac
else
    echo "   Cannot read setting"
fi

echo ""
echo "4. KASLR status:"
grep -q nokaslr /proc/cmdline && echo "   KASLR disabled" || echo "   KASLR enabled (addresses randomized)"

echo ""
echo "5. Struct offsets (these are NOT in symbols):"
echo "   Must be determined by:"
echo "   - Kernel headers (linux-headers package)"
echo "   - Debug info (linux-image-*-dbg package)"
echo "   - Reverse engineering"
echo "   - Pre-built offset databases"