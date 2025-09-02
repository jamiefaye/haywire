# QEMU Rebuild Checklist

Follow these steps every time you modify and rebuild QEMU:

## 1. Build the Binary
```bash
cd /Users/jamie/haywire/qemu-mods/qemu-src
ninja -C build
```

## 2. Copy to Deployment Location
```bash
cd /Users/jamie/haywire/qemu-mods
cp qemu-src/build/qemu-system-aarch64-unsigned qemu-system-aarch64-unsigned
```

## 3. Sign with HVF Entitlement
```bash
codesign --sign - --force --entitlements entitlements.plist qemu-system-aarch64-unsigned
```

## 4. Restart the VM
Kill any running QEMU instance and restart using:
```bash
/Users/jamie/haywire/scripts/launch_qemu_membackend.sh
```

## Important Notes
- The entitlements.plist file MUST include `com.apple.security.hypervisor` for HVF acceleration
- Without proper signing, you'll get: "Could not access HVF"
- The binary must be in `/Users/jamie/haywire/qemu-mods/` for the launch script to find it
- Always use `qemu-system-aarch64-unsigned` as the binary name

## Quick One-Liner
```bash
cd /Users/jamie/haywire/qemu-mods/qemu-src && \
ninja -C build && \
cd .. && \
cp qemu-src/build/qemu-system-aarch64-unsigned . && \
codesign --sign - --force --entitlements entitlements.plist qemu-system-aarch64-unsigned
```