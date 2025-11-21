# Archived Scripts

This directory contains obsolete or variant scripts that are no longer actively maintained but kept for historical reference.

## Contents

### Alpine Linux Variants
- `launch_alpine_vlc.sh` - Alpine Linux with VLC
- `alpine_quick_setup.sh` - Alpine quick setup
- `setup_alpine_vlc.sh` - Alpine VLC setup

### QEMU Configuration Variants
- `launch_qemu_clean.sh` - Old clean configuration
- `launch_qemu_membackendB4.sh` - Memory backend backup/old version
- `launch_qemu_overlay.sh` - Overlay disk variant
- `launch_qemu_tcg.sh` - TCG (slow emulation) mode
- `launch_qemu_vlc.sh` - VLC-specific QEMU config

### Ubuntu Variants
- `launch_ubuntu_arm64.sh` - Duplicate of main ARM64 script
- `launch_ubuntu_arm64_gdb.sh` - GDB debugging variant
- `launch_ubuntu_fast.sh` - Old fast boot variant
- `launch_ubuntu_highmem_off.sh` - highmem=off test variant
- `launch_ubuntu_vlc.sh` - VLC-specific Ubuntu config
- `launch_ubuntu_x86_64_macos.sh` - Intel Mac variant (less common now)

### Test Variants
- `launch_simple_vlc.sh` - Simple VLC test
- `launch_tinycore_vlc.sh` - Tiny Core Linux variant

### Deployment Scripts
- `deploy_and_test.sh` - Deployment test script
- `deploy_camera.sh` - Camera deployment script
- `serve_files.sh` - File serving utility
- `vlc_minimal.sh` - Minimal VLC config

## Why Archived?

These scripts were created during development for:
- Testing different Linux distributions (Alpine, Tiny Core)
- Experimenting with QEMU configurations
- VLC-specific media playback testing
- Different acceleration modes
- Development iterations

The canonical scripts in the parent directory (`launch_linux_arm64_macos.sh`, etc.) supersede these variants.

## Should You Use These?

**No.** Use the canonical scripts in the parent directory instead.

These are kept only for:
- Understanding the project's evolution
- Reference for specific edge cases
- Historical documentation

If you need a specific feature from one of these scripts, consider adding it to the canonical scripts instead.
