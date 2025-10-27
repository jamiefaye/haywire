# WSL2 Setup Pickup Guide - October 27, 2025

## Current Status

### ✅ What's Working:
1. **WSL2 with Ubuntu** - Running with KVM support enabled
2. **Modified QEMU 9.1.0** - Built successfully at `~/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64`
3. **Build Dependencies** - All installed (gcc, cmake, OpenGL, etc.)
4. **Git Branch** - Created `windows-wsl2-support` branch for Windows/Linux fixes
5. **Documentation** - Complete WSL2 setup guide at `docs/windows_deployment.md`
6. **Launch Script** - Ready at `scripts/launch_ubuntu_x86_64_linux.sh`

### ⚠️ What Needs Fixing:
1. **Haywire C++ compilation errors** - Minor header file organization issues
   - Problem: inline methods trying to access members before they're declared
   - Files: `include/address_space_flattener.h`
   - Solution: Move member declarations before inline methods

## Quick Resume Commands

### Step 1: Fix Compilation Issues

Open in Windows:
```
C:\Users\jamie\haywire\include\address_space_flattener.h
```

**Change lines 161-171 from:**
```cpp
// Callback when navigation changes
using NavigationCallback = std::function<void(uint64_t)>;

private:
    AddressSpaceFlattener* flattener;
    uint64_t currentVirtualAddr;
    uint64_t currentFlatAddr;
    NavigationCallback callback;

public:
    void SetNavigationCallback(NavigationCallback cb) { callback = cb; }
```

**To:**
```cpp
// Callback when navigation changes
using NavigationCallback = std::function<void(uint64_t)>;

private:
    AddressSpaceFlattener* flattener;
    uint64_t currentVirtualAddr;
    uint64_t currentFlatAddr;
    NavigationCallback callback;
    float sliderPos;  // 0.0 to 1.0
    bool isDragging;

public:
    void SetNavigationCallback(NavigationCallback cb) { callback = cb; }
```

**And remove the duplicate private section at the bottom (lines 175-178):**
```cpp
private:
    // UI state
    float sliderPos;  // 0.0 to 1.0
    bool isDragging;
```

### Step 2: Copy Fixed Header to WSL2

```bash
wsl bash -c "cp /mnt/c/Users/jamie/haywire/include/address_space_flattener.h ~/haywire/include/"
```

### Step 3: Build Haywire

```bash
wsl bash -c "cd ~/haywire && rm -rf build && mkdir build && cd build && cmake .. && make -j\$(nproc)"
```

**Expected time:** 2-3 minutes

**Success indicator:**
```
[100%] Built target haywire
```

### Step 4: Test Binary

```bash
wsl bash -c "~/haywire/build/haywire --help"
```

Should show Haywire help/version info.

## Next Steps After Haywire Builds

### 1. Download Ubuntu x86_64 ISO (Optional - if you want to create a VM)

```bash
wsl bash -c "cd ~/haywire/vms && wget https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso"
```

**Size:** ~2.6GB
**Time:** 5-10 minutes depending on connection

### 2. Create and Launch VM

```bash
wsl bash -c "cd ~/haywire/scripts && ./launch_ubuntu_x86_64_linux.sh"
```

First run will:
- Create 40GB qcow2 disk
- Boot from ISO
- Let you install Ubuntu

After install, shutdown VM and run script again to boot from disk.

### 3. Extract Kernel Profile from VM

Inside the VM (via console):
```bash
sudo apt-get install dwarves
pahole -C task_struct /sys/kernel/btf/vmlinux > /tmp/profile.txt
pahole -C mm_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
```

Copy output to host (you can cat and copy-paste), then:
```bash
wsl bash -c "cd ~/haywire && python3 scripts/create_kernel_profile.py < /tmp/profile.txt > profiles/ubuntu-$(uname -r)-x86_64.json"
```

### 4. Run Haywire

In one WSL2 terminal:
```bash
wsl bash -c "cd ~/haywire/scripts && ./launch_ubuntu_x86_64_linux.sh"
```

In another WSL2 terminal (or just open WSL):
```bash
wsl
cd ~/haywire/build
./haywire
```

**WSLg will display the GUI as a Windows window!**

## Troubleshooting

### If Haywire build still fails:

Create an issue with the full error:
```bash
wsl bash -c "cd ~/haywire/build && cmake .. && make -j\$(nproc)" 2>&1 | tee build.log
```

Then share `build.log`

### If KVM doesn't work:

Check KVM availability:
```bash
wsl ls -la /dev/kvm
wsl groups | grep kvm
```

If not in kvm group:
```bash
wsl sudo usermod -a -G kvm $USER
# Then restart WSL: wsl --shutdown, then wsl again
```

### If QEMU not found:

Verify QEMU binary:
```bash
wsl ls -la ~/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64
wsl ~/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64 --version
```

## File Locations Reference

**In WSL2 Linux filesystem:**
- Haywire source: `~/haywire/`
- Haywire binary: `~/haywire/build/haywire`
- Modified QEMU: `~/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64`
- VM disks: `~/haywire/vms/`
- Memory file: `/tmp/haywire-vm-mem` (created by QEMU when VM runs)
- QMP socket: `localhost:4445`

**In Windows:**
- Source (synced): `C:\Users\jamie\haywire\`
- Access WSL files: `\\wsl$\Ubuntu\home\jamie\haywire\`

## Key Configuration

**Current branch:** `windows-wsl2-support`

**Your credentials:**
- WSL2 user: `jamie`
- WSL2 password: `p`

**VM will use:**
- SSH port: 2222 (forwarded to localhost)
- QMP port: 4445
- Monitor port: 4444

## Estimated Time to Complete

From current state:
1. Fix header file: **2 minutes**
2. Build Haywire: **2-3 minutes**
3. Download Ubuntu ISO: **5-10 minutes** (optional)
4. Install Ubuntu in VM: **10-15 minutes** (optional)
5. Extract kernel profile: **2 minutes**
6. **Total: 15-30 minutes to fully working system**

## Alternative: Skip VM Creation

If you just want to test Haywire compiles, you can:
1. Fix header + build Haywire
2. Run `./haywire` without a VM (will show connection error - that's expected)
3. This confirms the build works

Then create VM later when you have time.

## Notes

- All builds happen in WSL2 native filesystem (`~/haywire`) for speed
- Windows source at `C:\Users\jamie\haywire` stays in sync via rsync when needed
- Commits made to `windows-wsl2-support` branch
- Can merge to `main` once tested working

## Summary

**You're 95% done!** Just need to:
1. Fix one header file (2 min edit)
2. Rebuild (3 min compile)
3. Test!

The hard part (QEMU build, dependencies, WSL2 setup) is complete. The remaining issue is a simple C++ member ordering fix.
