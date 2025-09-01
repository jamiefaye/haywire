#!/bin/bash
# Update the patch file with current changes

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
QEMU_DIR="$SCRIPT_DIR/qemu-src"

if [ ! -d "$QEMU_DIR" ]; then
    echo "ERROR: QEMU source not found. Run ./dev-setup.sh first"
    exit 1
fi

cd "$QEMU_DIR"

echo "=== Updating patch file ==="
echo "Current changes:"
git diff --stat

echo ""
echo "Creating new patch..."
git diff > "$SCRIPT_DIR/qemu-va2pa.patch"

echo "Patch updated successfully!"
echo ""
echo "To commit your changes:"
echo "  cd $SCRIPT_DIR/.."
echo "  git add qemu-mods/qemu-va2pa.patch"
echo "  git commit -m 'Update QEMU VA->PA translation'"