#!/bin/bash

# Check where video memory might be in QEMU guest

echo "=== Checking QEMU video memory configuration ==="

# Check if QEMU is running
if pgrep -x "qemu-system" > /dev/null; then
    echo "QEMU is running"
    
    # Try to get framebuffer info via QMP
    echo ""
    echo "=== QMP: Display info ==="
    echo '{"execute":"qmp_capabilities"}{"execute":"query-display-options"}' | nc localhost 4445 2>/dev/null | grep -v "QMP"
    
    echo ""
    echo "=== QMP: Memory regions ==="
    echo '{"execute":"qmp_capabilities"}{"execute":"query-memory-devices"}' | nc localhost 4445 2>/dev/null | grep -v "QMP"
    
    # Check monitor for VGA info
    echo ""
    echo "=== Monitor: VGA info ==="
    echo "info vga" | nc localhost 4444 2>/dev/null | head -20
    
    # Check for ramfb (RAM framebuffer)
    echo ""
    echo "=== Monitor: Ramfb info ==="
    echo "info ramfb" | nc localhost 4444 2>/dev/null | head -20
    
    # Check PCI devices for video
    echo ""
    echo "=== Monitor: PCI devices (video) ==="
    echo "info pci" | nc localhost 4444 2>/dev/null | grep -A2 -i "vga\|display\|gpu"
    
else
    echo "QEMU is not running"
fi

echo ""
echo "=== Typical video memory locations ==="
echo "VGA:     0xA0000 - 0xBFFFF    (128KB legacy VGA)"  
echo "VESA:    0xE0000000+          (VESA framebuffer)"
echo "virtio:  0x40000000+          (virtio-gpu)"
echo "ramfb:   Dynamic              (RAM framebuffer)"