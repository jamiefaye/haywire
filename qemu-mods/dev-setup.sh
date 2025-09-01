#!/bin/bash
# Set up QEMU development environment for iterating on changes

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
QEMU_DIR="$SCRIPT_DIR/qemu-src"

echo "=== Setting up QEMU development environment ==="

# Use existing backup if available
if [ -d "$HOME/qemu-backup" ] && [ ! -d "$QEMU_DIR" ]; then
    echo "Moving existing QEMU source to qemu-mods..."
    mv "$HOME/qemu-backup" "$QEMU_DIR"
    echo "Moved successfully!"
elif [ -d "$QEMU_DIR" ]; then
    echo "QEMU source already exists at $QEMU_DIR"
else
    echo "Cloning fresh QEMU..."
    git clone --depth 1 --branch v9.1.0 https://gitlab.com/qemu-project/qemu.git "$QEMU_DIR"
fi

echo ""
echo "=== Development environment ready! ==="
echo ""
echo "To make changes:"
echo "  1. Edit files in: $QEMU_DIR"
echo "  2. Quick rebuild: ./quick-rebuild.sh"
echo "  3. Test changes"
echo "  4. When happy, update patch: ./update-patch.sh"
echo ""
echo "Key files to edit:"
echo "  - $QEMU_DIR/qapi/misc.json"
echo "  - $QEMU_DIR/target/arm/arm-qmp-cmds.c"