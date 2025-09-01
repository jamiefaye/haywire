# QEMU Modifications for Haywire

This directory contains patches and build scripts for QEMU with VA->PA translation support.

## Structure

- `qemu-va2pa.patch` - Our modifications to QEMU
- `build-qemu.sh` - Automated build script
- `qemu-system-aarch64-unsigned` - Pre-built binary (can be regenerated)

## Changes Made

1. Added QMP command `query-va2pa` for virtual-to-physical address translation
2. Files modified in QEMU source:
   - `qapi/misc.json` - Added command definition
   - `target/arm/arm-qmp-cmds.c` - Added implementation

## Usage

```json
{
  "execute": "query-va2pa",
  "arguments": {
    "cpu-index": 0,
    "addr": 0xffff000000000000
  }
}
```

Returns:
```json
{
  "return": {
    "phys": 0x40000000,
    "size": 4096,
    "valid": true
  }
}
```

## Building from Source

Simply run:
```bash
./build-qemu.sh
```

This will:
1. Clone QEMU (if needed)
2. Apply our patches
3. Build QEMU with ARM64 support
4. Sign the binary for macOS
5. Install it in this directory

## Manual Build

If you prefer to build manually, the script automates these steps:
1. Clone QEMU v9.1.0
2. Apply `qemu-va2pa.patch`
3. Configure: `./configure --target-list=aarch64-softmmu --enable-debug`
4. Build: `ninja -C build`
5. Sign: `codesign --entitlements accel/hvf/entitlements.plist --force -s - build/qemu-system-aarch64-unsigned`