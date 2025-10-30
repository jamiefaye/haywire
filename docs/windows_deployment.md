# Deploying Haywire on Windows (Native x86_64)

## Goal

Run Haywire on a Windows Intel/AMD system to analyze native x86_64 Linux VMs without emulation overhead.

## Quick Start (WSL2 + KVM - RECOMMENDED)

**Best option for Windows users:** Run QEMU with KVM inside WSL2, compile and run Haywire natively in WSL2.

**Requirements:**
- Windows 11 (or Windows 10 with recent updates)
- Intel CPU with VT-x or AMD CPU with AMD-V
- At least 8GB RAM (12GB+ recommended)

**5-Minute Setup:**
```powershell
# 1. Install WSL2 (PowerShell as Administrator)
wsl --install -d Ubuntu-22.04

# 2. Enable nested virtualization (PowerShell as Administrator)
Set-VMProcessor -VMName WSL -ExposeVirtualizationExtensions $true
```

```bash
# 3. Inside WSL2 - Install dependencies
sudo apt-get update
sudo apt-get install -y build-essential cmake git qemu-system-x86 libcapstone-dev ninja-build

# 4. Clone Haywire (if not already)
cd ~
git clone https://github.com/yourusername/haywire.git
cd haywire

# 5. Build modified QEMU (one-time setup)
cd qemu-mods/qemu-src
mkdir build && cd build
../configure --target-list=x86_64-softmmu
make -j$(nproc)
sudo make install  # Or use from build directory

# 6. Build Haywire
cd ~/haywire
mkdir build && cd build
cmake ..
make -j$(nproc)

# 7. Launch Ubuntu x86_64 VM
cd ~/haywire/scripts
./launch_ubuntu_x86_64_linux.sh

# 8. Run Haywire (in another WSL2 terminal)
cd ~/haywire/build
./haywire
```

See detailed instructions below for troubleshooting and alternatives.

## Why Windows x86_64?

**Performance Benefits:**
- Native CPU execution (no ARM64 emulation)
- Faster VM performance
- More realistic production environment
- Broader hardware compatibility

**Use Cases:**
- Security analysis of x86_64 Linux servers
- Malware analysis in controlled VMs
- Memory forensics on common platforms
- CTF/training environments

## Architecture Comparison

| Platform | Host CPU | Guest CPU | Emulation | Speed |
|----------|----------|-----------|-----------|-------|
| macOS (current) | Apple Silicon ARM64 | Ubuntu ARM64 | None* | Good |
| macOS + VMware | Apple Silicon ARM64 | Ubuntu ARM64 | Some† | Moderate |
| Windows x86_64 | Intel/AMD x86_64 | Ubuntu x86_64 | None | **Excellent** |

*QEMU on Apple Silicon can run ARM64 natively
†VMware on Apple Silicon has some emulation overhead

## Deployment Options

### Option 1: QEMU with WHPX (RECOMMENDED)

**Advantages:**
- Same memory-backend-file as macOS (identical workflow!)
- Same QMP interface for kernel introspection
- Native performance with WHPX acceleration
- Live memory access (not snapshots)
- Free and open source

**Requirements:**
- Windows 10/11 (any edition)
- Enable "Windows Hypervisor Platform" feature
- Intel CPU with VT-x or AMD CPU with AMD-V

**Steps:**
1. Enable Windows Hypervisor Platform:
   - Control Panel → Programs → Turn Windows features on or off
   - Check "Windows Hypervisor Platform"
   - Reboot
2. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
3. Use provided script: `scripts\launch_ubuntu_x86_64_windows.bat`
4. Extract kernel profile from guest
5. Run Haywire in WSL2 or compile native

**IMPORTANT:** WHPX has known bugs with Linux guests:
- MSI injection failures (constant error spam)
- MMIO emulation errors
- Requires `-accel whpx,kernel-irqchip=off` workaround
- Still unstable even with workarounds (as of 2024)

**Script:** `scripts/launch_ubuntu_x86_64_windows.bat` (use only if WSL2+KVM doesn't work)

### Option 2: WSL2 + KVM (BEST PERFORMANCE - RECOMMENDED)

**Advantages:**
- KVM performance (fastest!)
- Linux environment for Haywire
- Same workflow as Linux server
- No Windows-specific issues
- WSLg provides X11 for Haywire GUI

**Requirements:**
- Windows 11 (WSL2 with nested virtualization)
- Intel CPU with VT-x or AMD CPU with AMD-V

**Detailed Setup:**

#### Step 1: Install WSL2
```powershell
# PowerShell as Administrator
wsl --install -d Ubuntu-22.04
# Reboot if prompted
```

#### Step 2: Enable Nested Virtualization
```powershell
# PowerShell as Administrator
Set-VMProcessor -VMName WSL -ExposeVirtualizationExtensions $true
```

#### Step 3: Verify KVM in WSL2
```bash
# Inside WSL2
ls -la /dev/kvm
# Should show: crw-rw---- 1 root kvm /dev/kvm

# Add yourself to kvm group
sudo usermod -a -G kvm $USER
# Logout and login to WSL2 for group change to take effect
```

#### Step 4: Install Build Dependencies
```bash
# Inside WSL2
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libcapstone-dev \
    qemu-system-x86 \
    ninja-build \
    pkg-config \
    libglib2.0-dev \
    libpixman-1-dev \
    libgtk-3-dev \
    libsdl2-dev
```

#### Step 5: Build Modified QEMU
```bash
cd ~/haywire/qemu-mods/qemu-src
mkdir build && cd build

# Configure for x86_64 target
../configure --target-list=x86_64-softmmu --enable-kvm

# Build (takes 5-10 minutes)
make -j$(nproc)

# Install system-wide (optional)
sudo make install

# OR use directly from build directory
# Path will be: ~/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64
```

#### Step 6: Download Ubuntu x86_64 ISO

**IMPORTANT:** Use Ubuntu Server, not Desktop. Desktop ISOs are very slow in VMs without GPU acceleration (VGA device causes 5+ minute black screens during boot).

```bash
cd ~/haywire
mkdir -p vms
cd vms

# Download Ubuntu 24.04 Server (RECOMMENDED - fast, reliable)
wget https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso

# Or Ubuntu 22.04 Server (more stable for older hardware)
wget https://releases.ubuntu.com/22.04/ubuntu-22.04.5-live-server-amd64.iso

# Desktop ISOs NOT recommended (too slow in VMs)
# Can install desktop packages after Server installation if needed
```

**IMPORTANT:** If you already have an ISO on Windows:
```bash
# Copy it to WSL2 native storage (do NOT use symlinks to /mnt/c/)
# QEMU may fail to boot from ISOs on Windows filesystem mounts
cp /mnt/c/path/to/your.iso ~/haywire/vms/ubuntu-24.04-desktop-amd64.iso

# This copies the file (takes ~30 seconds for 6GB ISO)
# but ensures reliable QEMU boot without CD-ROM read errors
```

#### Step 7: Build Haywire
```bash
cd ~/haywire
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# Test it built correctly
./haywire --help
```

#### Step 8: Launch VM
```bash
cd ~/haywire/scripts
./launch_ubuntu_x86_64_linux.sh

# First run will create VM disk and boot from ISO
# Install Ubuntu, then shutdown and rerun script to boot from disk
```

#### Step 9: Extract Kernel Profile (One-time per kernel version)
```bash
# Inside the Ubuntu VM (via VM console):
sudo apt-get install dwarves
pahole -C task_struct /sys/kernel/btf/vmlinux > /tmp/profile.txt
pahole -C mm_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt

# Copy to host (via shared folder or copy-paste)
# Then on WSL2 host:
cd ~/haywire
python3 scripts/create_kernel_profile.py < /tmp/profile.txt > profiles/ubuntu-6.14.0-x86_64.json
```

#### Step 10: Run Haywire
```bash
# In WSL2 terminal (WSLg will display GUI)
cd ~/haywire/build
./haywire

# Memory file is at: /tmp/haywire-vm-mem
# QMP port: 4445
# Should auto-detect and connect
```

**File System Layout in WSL2:**
```
~/haywire/
├── build/haywire              # Compiled binary
├── vms/
│   ├── ubuntu_x86_64.qcow2   # VM disk
│   └── ubuntu-24.04-*.iso    # Install ISO
├── qemu-mods/qemu-src/build/
│   └── qemu-system-x86_64    # Modified QEMU
├── profiles/
│   └── ubuntu-6.14.0-x86_64.json  # Kernel profile
└── scripts/
    └── launch_ubuntu_x86_64_linux.sh

/tmp/haywire-vm-mem            # Memory-mapped file (created by QEMU)
/tmp/qga.sock                  # Guest agent socket
```

**WSLg Note:** Windows 11 includes WSLg (WSL GUI), which automatically forwards X11 apps to Windows. Haywire's ImGui window will appear as a native Windows window!

**Script:** Use `scripts/launch_ubuntu_x86_64_linux.sh` in WSL2

### WSL2 Troubleshooting

#### Issue: $HOME Points to Windows Path
**Symptoms:** Scripts fail with errors like `/c/Users/jamie/...` not found

**Cause:** WSL2 may set `$HOME` to Windows path format instead of Linux format

**Solution:** The launch script now uses `HOME_DIR=$(cd ~ && pwd)` to get the correct Linux path. If you encounter issues with custom scripts:
```bash
# Check your $HOME
echo $HOME  # May show /c/Users/... (wrong) or /home/... (correct)

# Use tilde or $(cd ~ && pwd) instead
HOME_DIR=$(cd ~ && pwd)
"$HOME_DIR/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64" ...
```

#### Issue: ISO Boot Failure / CD-ROM Not Found
**Symptoms:** VM can't boot from Ubuntu installer ISO

**Solutions:**
1. **Copy ISO to native WSL2 storage** (don't use symlinks to `/mnt/c/`):
   ```bash
   cp /mnt/c/path/to/ubuntu.iso ~/haywire/vms/ubuntu-24.04-desktop-amd64.iso
   ```

2. **Delete empty disk if first boot failed:**
   ```bash
   rm ~/haywire/vms/ubuntu_x86_64.qcow2
   # Then relaunch - script will recreate disk and boot from ISO
   ```

3. **Verify ISO is readable:**
   ```bash
   file ~/haywire/vms/ubuntu-24.04-desktop-amd64.iso
   # Should say "ISO 9660 CD-ROM filesystem" (not "symbolic link")
   ```

### Option 3: VirtualBox (If needed for specific use case)

**Advantages:**
- Free and open source
- Works on Windows/Mac/Linux
- Open source (could add memory-backend-file feature)

**Disadvantages:**
- Only supports snapshot-based introspection (not live)
- Requires taking snapshots periodically
- More complex setup

**Not recommended** unless you need VirtualBox for other reasons.

## Compilation on Windows

### Method 1: WSL2 (Recommended)

**Compile Linux binary in WSL2, run against Windows-hosted VMs:**

```bash
# In Windows PowerShell (install WSL2):
wsl --install -d Ubuntu-22.04

# In WSL2:
sudo apt-get update
sudo apt-get install -y build-essential cmake git
cd /mnt/c/Users/YourName/haywire  # Access Windows files
mkdir build && cd build
cmake ..
make
```

**Access VM memory files:**
```bash
# VMware .vmem files:
ls /mnt/c/Users/YourName/Documents/Virtual\ Machines/Ubuntu/Ubuntu.vmem

# Hyper-V .bin files:
ls /mnt/c/ProgramData/Microsoft/Windows/Hyper-V/...

# Create symlink:
ln -sf /mnt/c/Users/.../Ubuntu.vmem /tmp/haywire-vm-mem
```

### Method 2: MinGW (Native Windows Binary)

**Compile native Windows .exe:**

```bash
# Install MSYS2 from https://www.msys2.org/
# In MSYS2 terminal:
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make

cd /c/Users/YourName/haywire
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
mingw32-make
```

**Note:** May need to port some Linux-specific code (mmap, file paths)

## Kernel Profile Differences

**x86_64 vs ARM64:**
- Different structure layouts
- Different pointer sizes (both 64-bit, but alignment differs)
- Different page table formats
- Different kernel VA ranges

**You'll need separate profiles:**
```
profiles/
├── ubuntu-6.14.0-34-arm64.json     # macOS/ARM64
├── ubuntu-6.14.0-34-x86_64.json    # Windows/Intel
└── ubuntu-6.8.0-48-x86_64.json     # Example: older kernel
```

## Testing Workflow

### 1. Validate on macOS VMware (Current Phase)

Get VMware working on your Mac with ARM64:
- Proves multi-platform concept
- Tests .vmem file reading
- Validates profile system
- Tests heuristic swapper_pgd discovery

### 2. Move to Windows x86_64 (Next Phase)

**Quick test setup:**
```bash
# 1. Install VMware Workstation Player on Windows
# 2. Create Ubuntu 24.04 x86_64 VM (4GB RAM)
# 3. Boot VM and extract profile
# 4. In WSL2:
cd /mnt/c/Users/YourName/haywire/build
./haywire
```

### 3. Production Deployment

**For actual use:**
- Dedicated analysis workstation (Windows PC)
- Multiple VMs for different analysis tasks
- Profiles for each kernel version you encounter
- Automated memory dump + analysis pipeline

## Memory File Locations

### VMware Workstation (Windows)
```
C:\Users\YourName\Documents\Virtual Machines\Ubuntu\Ubuntu.vmem
```

### Hyper-V (Windows)
```
C:\ProgramData\Microsoft\Windows\Hyper-V\Virtual Machines\{VM-GUID}\Memory.bin
```

### VirtualBox (Windows)
```
C:\Users\YourName\VirtualBox VMs\Ubuntu\Snapshots\{snapshot}.sav
```

## Next Steps

1. **Finish macOS VMware testing** - validates the approach
2. **Get Windows PC ready** - install VMware/VirtualBox
3. **Create x86_64 Ubuntu VM** - native performance
4. **Extract x86_64 profile** - different offsets than ARM64
5. **Compile Haywire in WSL2** - easiest path
6. **Test against x86_64 VM** - full validation

## Performance Expectations

**macOS ARM64 → Windows x86_64 improvements:**
- VM performance: 2-3x faster (native vs emulated)
- Memory scanning: Similar (I/O bound)
- Page table walking: Slightly faster (simpler x86_64 page tables)
- Overall: Much more responsive for interactive use

## Questions to Answer

- [ ] Will you use VMware, Hyper-V, or VirtualBox on Windows?
- [ ] Do you have a Windows x86_64 machine available?
- [ ] What Ubuntu version will you run (24.04 LTS recommended)?
- [ ] WSL2 or native Windows compilation?

Once you answer these, we can create a detailed step-by-step guide for your specific setup.
