# Haywire TODO List

## Next Week: VM Setup Guides & Documentation

### 1. Linux Guest Setup Guides

#### Linux ARM64 on macOS (Apple Silicon)
- [ ] Create `docs/setup_linux_arm64_macos.md`
- [ ] Step-by-step Ubuntu 24.04 ARM64 installation
  - [ ] Download Ubuntu ARM64 ISO
  - [ ] Create qcow2 disk image
  - [ ] Install Ubuntu with QEMU
  - [ ] Install guest agent (qemu-guest-agent)
  - [ ] Setup SSH with key authentication
  - [ ] Install development tools (pahole, dwarves)
  - [ ] Create kernel profile for Haywire
- [ ] Include troubleshooting section
- [ ] Add verification commands at each step

#### Linux x86_64 on Windows (WHPX)
- [ ] Create `docs/setup_linux_x86_64_windows.md`
- [ ] Step-by-step Ubuntu 24.04 x86_64 installation on Windows
  - [ ] Enable Windows Hypervisor Platform (WHPX)
  - [ ] Install QEMU for Windows
  - [ ] Download Ubuntu x86_64 ISO
  - [ ] Create qcow2 disk image
  - [ ] Install Ubuntu with QEMU + WHPX
  - [ ] Install guest agent
  - [ ] Setup SSH and port forwarding
  - [ ] Install development tools
  - [ ] Create kernel profile
- [ ] Windows-specific quirks (paths, named pipes)
- [ ] Performance tuning tips

### 2. Windows Guest Setup Guides

#### Windows 11 x86_64 on macOS (Apple Silicon) - COMPLETED ✅
- [x] Script working: `launch_windows_x86_64_macos.sh`
- [x] Documented in `scripts/README.md`
- [ ] Create detailed `docs/setup_windows_x86_64_macos.md`
  - [ ] Prerequisites (Homebrew, QEMU, swtpm)
  - [ ] Download Windows 11 ISO
  - [ ] Create qcow2 disk image
  - [ ] Install Windows with TPM 2.0 emulation
  - [ ] OVMF firmware setup
  - [ ] First boot configuration
  - [ ] Windows Update handling
  - [ ] Performance tips

#### Windows 11 x86_64 on Windows (WHPX)
- [ ] Create `docs/setup_windows_x86_64_windows.md`
- [ ] Native Windows host → Windows guest
  - [ ] Enable WHPX
  - [ ] Install QEMU for Windows
  - [ ] Download Windows 11 ISO
  - [ ] TPM 2.0 setup with swtpm (Windows build)
  - [ ] Create and configure VM
  - [ ] Nested virtualization considerations
  - [ ] Performance optimization

### 3. Future: Windows ARM64 on macOS

#### Windows 11 ARM64 on macOS (Apple Silicon) - Native HVF
- [ ] Research Windows 11 ARM64 ISO availability
- [ ] Create `docs/setup_windows_arm64_macos.md`
- [ ] Installation guide
  - [ ] Download Windows 11 ARM64 ISO
  - [ ] HVF acceleration (native speed!)
  - [ ] TPM 2.0 requirements
  - [ ] UEFI ARM64 firmware
  - [ ] Install and configure
- [ ] Test Haywire with Windows ARM64
- [ ] Document Windows ARM64 kernel structures
- [ ] Create `launch_windows_arm64_macos.sh` script

### 4. Setup Automation & User Experience

#### Documentation Improvements
- [ ] Create `docs/QUICKSTART.md` - 5-minute overview
- [ ] Create `docs/TROUBLESHOOTING.md` - Common issues
- [ ] Add screenshots/videos to guides
- [ ] Create decision tree: "Which VM should I use?"

#### Interactive Setup Tools
- [ ] Consider setup wizard script
  - [ ] Detect host platform (macOS/Windows/Linux)
  - [ ] Detect CPU architecture (ARM64/x86_64)
  - [ ] Recommend optimal VM configuration
  - [ ] Check prerequisites (QEMU, swtpm, etc.)
  - [ ] Download ISOs if needed
  - [ ] Guide through installation

#### Claude Integration Ideas
- [ ] Create `.claude/commands/setup-help.md` - Setup assistance command
- [ ] Structure guides for easy Claude parsing
- [ ] Add "Ask Claude" sections in docs
- [ ] Consider setup buddy chatbot integration

### 5. Quality of Life Improvements

#### Script Enhancements
- [ ] Add `--help` to all launch scripts
- [ ] Add VM status checking (is VM running?)
- [ ] Add cleanup commands (stop VM, remove disk, etc.)
- [ ] Add disk management (snapshot, backup, restore)

#### Build & Development Setup
- [ ] Create `docs/BUILD.md` - Building Haywire from source
  - [ ] Prerequisites for macOS
  - [ ] Prerequisites for Linux
  - [ ] Prerequisites for Windows
  - [ ] CMake build instructions
  - [ ] IDE setup (VSCode, CLion, etc.)

#### Testing & CI
- [ ] Document testing procedures
- [ ] Create test VM configurations
- [ ] Add CI/CD for building on multiple platforms

## Completed This Week ✅

### Windows 11 x86_64 on macOS (Apple Silicon)
- [x] Fixed TPM 2.0 requirement (swtpm integration)
- [x] Fixed OVMF firmware size limits (Linux firmware files)
- [x] Fixed VARS file configuration
- [x] Fixed QEMU configuration (match Linux script)
- [x] Confirmed working: 129 processes discovered
- [x] Documented in `scripts/README.md`
- [x] Performance validation (usable with native window)

### Script Organization
- [x] Renamed scripts for clarity
  - [x] `launch_linux_arm64_macos.sh`
  - [x] `launch_linux_x86_64_linux.sh`
  - [x] `launch_windows_x86_64_linux.sh`
  - [x] `launch_windows_x86_64_macos.sh`
- [x] Archived 20 obsolete scripts
- [x] Created `scripts/README.md` with platform support matrix
- [x] Created `scripts/archive/README.md`

### Debug Logging
- [x] Added `--debug` flag to control verbose output
- [x] Fixed maple tree infinite loop (node visit counter)
- [x] Clean minimal output by default
- [x] Full verbose output with `--debug`

## Platform Status Summary

| Guest OS | Host Platform | Status | Script |
|----------|---------------|--------|--------|
| Linux ARM64 | macOS (Apple Silicon) | ✅ Fast (HVF) | `launch_linux_arm64_macos.sh` |
| Linux x86_64 | Linux (Intel/AMD) | ✅ Fast (KVM) | `launch_linux_x86_64_linux.sh` |
| Windows 11 x86_64 | Linux (Intel/AMD) | ✅ Fast (KVM) | `launch_windows_x86_64_linux.sh` |
| Windows 11 x86_64 | macOS (Apple Silicon) | ✅ Usable (TCG) | `launch_windows_x86_64_macos.sh` |
| Windows 11 ARM64 | macOS (Apple Silicon) | 🔲 TODO | TBD |

## Notes

### Setup Guide Structure Template
Each guide should follow this structure:
1. **Prerequisites** - What you need before starting
2. **Download ISO** - Where to get it, which version
3. **Install QEMU & Tools** - Platform-specific installation
4. **Create Disk Image** - qemu-img commands
5. **First Boot & Install** - OS installation process
6. **Post-Install Setup** - Guest tools, SSH, development tools
7. **Launch Script** - Which script to use
8. **Verify with Haywire** - Test memory introspection
9. **Troubleshooting** - Common issues and fixes
10. **Next Steps** - What to do now

### Documentation Best Practices
- ✅ Clear, numbered steps
- ✅ Copy-pasteable commands
- ✅ Verification commands after each major step
- ✅ Screenshots where helpful
- ✅ "What you should see" descriptions
- ✅ Troubleshooting subsections
- ✅ Time estimates for each section
- ✅ "Ask Claude" prompts for complex steps

### Claude Assistance Strategy
1. Make documentation exceptional (reduce need for help)
2. Structure guides for easy Claude parsing
3. Include common error messages with solutions
4. Add "If you see X, try Y" patterns
5. Consider creating dedicated setup assistant command
6. Test guides with fresh users to find pain points

## Future Ideas (Backlog)

### Advanced Features
- [ ] Multiple VM management (track multiple VMs)
- [ ] VM snapshots and restore
- [ ] Automated kernel profile generation
- [ ] Cross-platform VM migration
- [ ] Windows ARM64 kernel discovery implementation
- [ ] Automated update checker for guest VMs

### Developer Experience
- [ ] VSCode integration
- [ ] Pre-built VM images (legal considerations)
- [ ] Docker-based build environment
- [ ] Automated testing with VMs
- [ ] Performance benchmarking suite

### Community
- [ ] Contribution guide
- [ ] Video tutorials
- [ ] Example use cases
- [ ] Blog posts about architecture
- [ ] Conference talk materials
