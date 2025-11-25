# Handoff to Windows 11 Dell - November 23, 2025

## Current Status

**All code committed and pushed to GitHub main branch.**

### Recent Fixes (Ready to Pull)
1. **PageDB Multi-Region RAM Fix** (commits: a08ee01, fc7f750, 42a1cd3)
   - Fixed initialization to sum all RAM regions (handles x86-64 PCI hole)
   - Improved GetPIDData to use virtualLookup for correct per-PID VAs
   - Tested on both Windows 11 x86-64 and Linux ARM64

2. **Windows Process Discovery** (commit: 1853543)
   - VAD tree walking with recursion protection
   - Process name character validation (space, tilde for Firefox threads)
   - Works with Windows 11 Build 26200.7171

## Setup on Windows 11 Dell

### Prerequisites
- Visual Studio 2022 (Community or Professional)
- CMake 3.20+
- Git for Windows
- VMware Workstation (for running Windows/Linux VMs to inspect)

### Clone and Build
```bash
cd C:\Users\YourUser\
git clone https://github.com/jamiefaye/haywire.git
cd haywire

# Create build directory
mkdir build
cd build

# Configure with Visual Studio generator
cmake .. -G "Visual Studio 17 2022" -A x64

# Build (or open haywire.sln in Visual Studio)
cmake --build . --config Release
```

### Testing Setup

**Option 1: VMware Workstation VMs**
- Windows VMs produce .vmem files (memory snapshots)
- Linux VMs also produce .vmem files
- Need to adapt MemoryFileReader to open .vmem instead of /tmp/haywire-vm-mem
- See profiles/README.md for VMware-specific instructions

**Option 2: QEMU on Windows (via WSL2)**
- Install WSL2 with Ubuntu
- Install QEMU in WSL2
- Memory file accessible from Windows at \\wsl$\Ubuntu\tmp\haywire-vm-mem
- Similar workflow to macOS testing

### Known Platform Differences

**macOS (where we just worked):**
- QEMU runs natively on Apple Silicon (ARM64 host)
- x86-64 guests use TCG emulation (slow but functional)
- ARM64 guests use HVF acceleration (fast)
- Memory file at `/tmp/haywire-vm-mem`

**Windows (target platform):**
- QEMU typically runs in WSL2 (Linux subsystem)
- Or use VMware Workstation (native Windows)
- Memory files: WSL2 path or .vmem files
- Need to test both approaches

## Current Test VMs

**macOS QEMU VMs (tested and working):**
1. Windows 11 x86-64 (8GB, Build 26200.7171)
   - Location: `~/haywire/vms/windows11.qcow2`
   - Launch: `scripts/launch_windows_x86_64_macos_nonet.sh`
   - Status: ✅ All 122 processes discovered, full memory maps

2. Ubuntu 24.04 ARM64 (8GB, kernel 6.14.0-36)
   - Location: `~/haywire/vms/ubuntu_arm64.qcow2`
   - Launch: `scripts/launch_ubuntu_arm64.sh`
   - Status: ✅ Process discovery and memory maps working

**Windows Dell Target:**
- Will use VMware Workstation VMs
- Need to create/configure test VMs there
- Can copy qcow2 files and convert to VMDK if needed

## Next Steps on Windows

1. **Pull latest code**
   ```bash
   git pull origin main
   ```

2. **Build with Visual Studio**
   - Open build/haywire.sln
   - Build in Release configuration
   - Or use cmake --build from command line

3. **Adapt for VMware**
   - Modify MemoryFileReader to open .vmem files
   - Test with VMware VM memory snapshots
   - See if live .vmem monitoring works (similar to memory-backend-file)

4. **Test Process Discovery**
   - Verify Windows 11 process discovery
   - Verify Linux process discovery
   - Check PageDB with both guest types

5. **Create Windows-Specific Launch Scripts**
   - Scripts to snapshot VMware VMs
   - Scripts to launch Haywire with correct .vmem path
   - Documentation for Windows workflow

## Key Files to Review

- `src/page_database.cpp` - Multi-region RAM init (lines 241-260 in main.cpp)
- `src/windows/windows_kernel_discovery.cpp` - VAD tree walking
- `include/page_database.h` - PhysToIndex PCI hole handling
- `profiles/windows/windows-11-26100-x86_64.json` - Windows offsets
- `CLAUDE.md` - Full project documentation

## Questions to Resolve

1. Does VMware .vmem file format match memory-backend-file layout?
2. Can we mmap .vmem files with MAP_SHARED for live updates?
3. Need to handle VMware's memory region layout (different from QEMU?)
4. Windows build differences (if any) from macOS/Linux builds

## Contact
All work documented in CLAUDE.md with full technical context.
Ready to continue development on Windows Dell.
