# VirtualBox Extension Pack - Quick Start Guide

## Ready to Start Next Week

This guide provides a step-by-step checklist for implementing the VirtualBox extension pack approach for Haywire.

## Week 1: Proof of Concept (macOS)

### Day 1-2: Setup and Skeleton

**Goal**: Get a minimal extension pack loading in VirtualBox

**Tasks**:
- [ ] Create extension pack directory structure
- [ ] Write minimal ExtPack.xml manifest
- [ ] Create stub HaywireExtPackMain.cpp (just logging)
- [ ] Build and install extension pack
- [ ] Verify it loads: `VBoxManage list extpacks`

**Directory Structure**:
```
~/haywire/vbox-extpack/
├── ExtPack.xml
├── ExtPack-license.html
├── ExtPack-license.txt
├── HaywireExtPackMain.cpp
├── Makefile
└── darwin.arm64/
    └── (built .dylib goes here)
```

**Success Criteria**: Extension pack appears in `VBoxManage list extpacks`

### Day 3-4: Shared Memory Initialization

**Goal**: Create shared memory file when VM starts

**Tasks**:
- [ ] Implement `pfnVMConfigureVMM` hook
- [ ] Access `pVM->pgm.s.cbRamSize` to get RAM size
- [ ] Create `/tmp/haywire-vm-mem` via `open()` + `mmap()`
- [ ] Verify file created with correct size: `ls -lh /tmp/haywire-vm-mem`
- [ ] Check VirtualBox logs for success messages

**Code Location**: `HaywireExtPackMain.cpp` - `pfnVMConfigureVMM()`

**Success Criteria**: File appears when VM starts, disappears when VM stops

### Day 5: Haywire Integration Test

**Goal**: Verify Haywire can read the shared memory file

**Tasks**:
- [ ] Start VM with extension pack
- [ ] Launch Haywire: `./build/haywire`
- [ ] Select `/tmp/haywire-vm-mem` as memory file
- [ ] Verify memory visualization appears
- [ ] Check if it's all zeros or has data

**Expected Result**: File opens, but memory is likely all zeros (no redirection yet)

**Important Note**: At this point we're creating the file, but VirtualBox is still allocating RAM normally. The memory won't appear in our file yet - that's Week 2's challenge.

## Week 2: GMM Allocator Hooking (Hard Part)

### Research Phase

**Goal**: Figure out how to redirect GMM chunk allocation from extension pack

**Investigation Tasks**:
- [ ] Study `PCVMMR3VTABLE` structure (passed to `pfnVMConfigureVMM`)
- [ ] Look for GMM-related function pointers we can replace
- [ ] Search for PGM/GMM callback registration functions
- [ ] Check if `pVM` structure has any allocator hooks

**Key Questions**:
1. Can we call `GMMR3RegisterChunkAllocator()` or similar?
2. Is there a `pVM->gmm.s.pfnAllocator` function pointer?
3. Can we override `RTR0MemObjAlloc*` from Ring-3?

**Documentation to Review**:
- `~/Downloads/VirtualBox-7.2.4/include/VBox/vmm/gmm.h`
- `~/Downloads/VirtualBox-7.2.4/include/VBox/vmm/pgm.h`
- `~/Downloads/VirtualBox-7.2.4/src/VBox/VMM/VMMR3/GMM.cpp`

### Implementation Approaches

**Option A: Function Pointer Replacement** (Most Likely)
```cpp
// If GMM has allocator function pointer in pVM structure:
pVM->gmm.s.pfnAllocateChunk = &OurSharedMemoryAllocator;
```

**Option B: VMM Registration API** (Cleanest)
```cpp
// If VMM provides registration function:
pVMM->pfnGMMRegisterAllocator(pVM, &OurAllocator, &OurDeallocator);
```

**Option C: Memory Backend Override** (Nuclear Option)
```cpp
// Override memory-backend-ram with memory-backend-file
// Similar to QEMU's approach
```

**Success Criteria**: Guest RAM allocations appear in `/tmp/haywire-vm-mem`

## Week 3: Testing and Polish

### Integration Testing

**Tasks**:
- [ ] Create Ubuntu VM (4GB RAM)
- [ ] Start VM with extension pack
- [ ] Verify 2048 chunks (4GB ÷ 2MB) allocated to shared file
- [ ] Run Haywire and discover kernel
- [ ] Test process discovery
- [ ] Verify memory contents match guest

### Cross-Platform Preparation

**Tasks**:
- [ ] Add Windows-specific code (`#ifdef RT_OS_WINDOWS`)
- [ ] Add Linux-specific code (`#ifdef __linux__`)
- [ ] Test builds on all platforms (if available)
- [ ] Create platform-specific .vbox-extpack files

## Week 4: Windows Deployment

### Windows Build Setup

**Prerequisites**:
- Visual Studio 2019 or later
- VirtualBox 7.2.4 installed
- VirtualBox SDK/headers

**Build Steps**:
```cmd
REM Set up Visual Studio environment
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

REM Build extension pack DLL
cl /LD /MD ^
    /I"C:\Program Files\Oracle\VirtualBox\sdk\bindings\mscom\include" ^
    /DVBOX_WITH_MAIN_NLS ^
    HaywireExtPackMain.cpp ^
    /link /DLL /OUT:win.amd64\HaywireExtPackMain.dll

REM Package extension pack
tar czf HaywireExtPack-win.vbox-extpack ExtPack.xml win.amd64\
```

### Testing on Windows

**Tasks**:
- [ ] Install extension pack on Windows
- [ ] Create x86_64 Ubuntu VM
- [ ] Verify shared memory: Check `Global\haywire-vm-mem`
- [ ] Compile Haywire for Windows (or use in WSL2)
- [ ] Full integration test

## Debugging Tips

### Extension Pack Not Loading

**Check**:
```bash
# Verify extension pack installed
VBoxManage list extpacks

# Check VirtualBox logs
tail -f ~/Library/Logs/VirtualBox/VBox.log | grep -i "extpack\|haywire"

# Enable verbose logging
export VBOX_LOG="+extpack.e.l.f"
```

### Shared Memory File Issues

**Troubleshooting**:
```bash
# Check if file created
ls -lh /tmp/haywire-vm-mem

# Monitor file access
sudo fs_usage -w -f filesys | grep haywire-vm-mem

# Verify mmap worked
vmmap <VBoxHeadless-PID> | grep haywire
```

### GMM Allocator Issues

**Check VMM Function Table**:
```cpp
LogRel(("Haywire: VMM version: %u\n", pVMM->uMagicVersion));
LogRel(("Haywire: VMM has %zu function pointers\n",
        sizeof(*pVMM) / sizeof(void*)));
// Print all function pointers to find allocator hooks
```

## Success Metrics

### Milestone 1: Extension Pack Loads
- ✅ Shows up in `VBoxManage list extpacks`
- ✅ Logs appear in VBox.log
- ✅ VM starts without errors

### Milestone 2: Shared Memory Created
- ✅ File appears at correct path
- ✅ Size matches VM RAM size
- ✅ File deleted when VM stops

### Milestone 3: Memory Redirection Works
- ✅ Guest RAM appears in shared file
- ✅ Haywire can read memory
- ✅ Kernel discovery works

### Milestone 4: Production Ready
- ✅ Works on macOS and Windows
- ✅ Stable across VM restarts
- ✅ Clean error handling
- ✅ Documentation complete

## Resources

### Documentation
- `docs/virtualbox_extpack_approach.md` - Complete implementation guide
- `docs/virtualbox_implementation_plan.md` - Original plan (now superseded)
- `include/VBox/ExtPack/ExtPack.h` - Extension Pack API reference

### Source Code
- `~/Downloads/VirtualBox-7.2.4/` - Full VirtualBox source
- `include/VBox/vmm/` - VMM headers
- `src/VBox/VMM/VMMR3/GMM.cpp` - GMM implementation
- `src/VBox/VMM/VMMR3/PGM.cpp` - PGM implementation

### Examples
- Look at existing extension packs in VirtualBox SDK
- Oracle Extension Pack (closed source, but documented)

## Estimated Timeline

| Phase | Duration | Confidence |
|-------|----------|------------|
| Extension pack skeleton | 2 days | 95% |
| Shared memory file creation | 2 days | 85% |
| GMM allocator hooking | 1 week | 40% |
| Testing and integration | 3 days | 70% |
| Windows port | 2 days | 80% |
| **Total** | **2-3 weeks** | **60%** |

**Key Risk**: GMM allocator hooking is the unknown. If we can't find a clean hook from the extension pack API, we may need to fall back to core patching or a different approach.

## Fallback Options

If extension pack approach blocks on GMM hooking:

1. **Snapshot-based introspection** - Use VirtualBox's existing .sav files (not live)
2. **Core patching** - Fall back to our original plan (higher complexity)
3. **QEMU on Windows** - Try WSL2+KVM one more time with proper setup
4. **Native Linux deployment** - Use Linux PC with VirtualBox patches

## Getting Help

When stuck, search VirtualBox forums/source for:
- "Extension pack memory"
- "GMM allocator callback"
- "PGM memory backend"
- "Custom memory allocator VirtualBox"

Or examine how existing extension packs (USB, PXE boot) hook into VirtualBox internals.

## Ready to Begin!

Next week, start with Day 1 tasks and work through systematically. The extension pack skeleton should be straightforward, and we'll tackle the GMM hooking challenge when we get there.

Good luck! 🚀
