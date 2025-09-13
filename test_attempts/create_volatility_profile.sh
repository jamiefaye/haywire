#!/bin/bash

echo "Creating Volatility-style profile for Ubuntu ARM64 kernel 6.14"
echo ""
echo "This would normally be done IN the guest VM:"
echo ""
echo "1. Install kernel headers:"
echo "   sudo apt-get install linux-headers-\$(uname -r)"
echo ""
echo "2. Get the Volatility tools:"
echo "   git clone https://github.com/volatilityfoundation/volatility.git"
echo "   cd volatility/tools/linux"
echo ""
echo "3. Build the profile:"
echo "   make"
echo "   sudo zip Ubuntu-6.14.0-29-generic-aarch64.zip module.dwarf /boot/System.map-6.14.0-29-generic"
echo ""
echo "The profile contains:"
echo "- module.dwarf: Struct offsets extracted from kernel debug info"
echo "- System.map: Symbol addresses"
echo ""
echo "But for our purposes, we can extract what we need directly:"

# What we actually need
cat << 'EOF'

Key information we need:
1. Struct offsets (from kernel headers or debug symbols)
2. Symbol addresses (from /proc/kallsyms or System.map)
3. Page table layout (ARM64 specific)

Quick extraction commands (run via guest agent):

# Get key symbol addresses
grep -E "(init_task|swapper_pg_dir|init_mm)" /proc/kallsyms

# Get struct offsets from debug info (if available)
# On Ubuntu: sudo apt install linux-image-$(uname -r)-dbgsym
# Then use: pahole -C task_struct /usr/lib/debug/boot/vmlinux-$(uname -r)

# Or use crash utility to get offsets
# sudo apt install crash
# crash /usr/lib/debug/boot/vmlinux-$(uname -r) /proc/kcore
# crash> struct task_struct | grep -E "(tasks|pid|comm|mm)"

EOF