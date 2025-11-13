# VirtualBox Memory Access - Final Plan

## The Problem We're Solving

**QEMU Issue:**
- `memory-backend-file` on x86_64 only exposes 0-4GB
- Kernel PGDs allocated at ~4.2-4.3GB physical addresses
- Result: 0% PGD extraction success in VA mode

**VirtualBox Advantage:**
- Dumps include 4GB+ memory (your investigation proved this)
- 66,067 PGD candidates found in high memory
- Kernel structures ARE accessible!

## What We've Discovered

### Memory Allocation Call Chain

```
Guest RAM Request
    ↓
GMMR0.cpp: gmmR0AllocateChunkNew()
    ↓
RTR0MemObjAllocPage(&hMemObj, GMM_CHUNK_SIZE=2MB, false)
    ↓
[Windows NT] memobj-r0drv-nt.cpp
    ↓
MmAllocatePagesForMdl() ← KERNEL MODE ALLOCATION
    ↓
Physical pages from Windows kernel
```

**Critical Discovery:** Allocations happen in **vboxdrv.sys** (kernel driver), not VirtualBox.exe!

### Why User-Mode Hooking Won't Work

- ❌ DLL injection into VirtualBox.exe → Wrong process, memory allocated in kernel
- ❌ COM API → ReadPhysicalMemory returns E_NOTIMPL
- ❌ Simple hooking → Can't hook kernel from user mode without privileges

## Available Approaches

### Option 1: Source Patch (RECOMMENDED)

**What:** Modify VirtualBox source to allocate chunks from shared memory file

**Files to modify:**
1. `src/VBox/Runtime/r0drv/nt/memobj-r0drv-nt.cpp` - Hook `MmAllocatePagesForMdl`
2. `src/VBox/VMM/VMMR3/PGM.cpp` - Create shared memory file
3. Configuration to enable it

**Advantages:**
- ✅ Zero latency (direct mmap)
- ✅ No driver signing needed (we compile our own signed driver)
- ✅ Clean, maintainable solution
- ✅ ~150 lines of code

**Disadvantages:**
- ⏱️ Must set up build environment (1 day)
- ⏱️ Compile VirtualBox (2-4 hours first time)
- 🔧 Must maintain across VirtualBox updates

**Effort:** 2-3 days total

---

### Option 2: Kernel Driver Hook

**What:** Create a separate kernel driver that hooks `vboxdrv.sys`

**Advantages:**
- ✅ No VirtualBox compilation needed
- ✅ Small standalone driver

**Disadvantages:**
- ⚠️ Requires Test Mode or self-signed certificate
- ⚠️ Windows will warn users about unsigned driver
- ⚠️ More fragile (hooking existing driver)
- ⚠️ May break on VirtualBox updates

**Effort:** 3-4 days (driver dev + testing)

---

### Option 3: Static Dumps (CURRENT WORKING)

**What:** Use your existing dump scripts in a loop

**Advantages:**
- ✅ Already working
- ✅ No compilation
- ✅ No driver signing

**Disadvantages:**
- ⏱️ High latency (1-10 second snapshots)
- 💾 High I/O (dumping 4GB+ repeatedly)
- 📊 Not suitable for real-time visualization

**Effort:** Already done!

---

## Recommendation: Source Patch

Given:
1. You already have VirtualBox source (`C:\Users\jamie\vbox-src`)
2. You want live, low-latency access
3. You're comfortable with C/C++ (you have Haywire working)
4. You want it to work reliably

**The source patch is the best approach.**

## Implementation Plan: Source Patch

### Phase 1: Setup Build Environment (4 hours)

Windows VirtualBox build requirements:
- Visual Studio 2019/2022 (you have this)
- Windows SDK
- Windows DDK (Driver Kit)
- kBuild (VirtualBox's build system)
- Python 3

### Phase 2: Implement Shared Memory Backend (4 hours)

**File 1: `src/VBox/VMM/VMMR3/PGM.cpp`**

Add initialization function:
```cpp
// Global shared memory state
static HANDLE g_hSharedMemFile = NULL;
static void* g_pSharedMemBase = NULL;
static size_t g_SharedMemSize = 0;
static size_t g_SharedMemOffset = 0;

int pgmR3InitSharedMemoryBackend(PVM pVM) {
    const size_t cbRam = pVM->pgm.s.cbRamSize;

    // Create memory-mapped file
    g_hSharedMemFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        (DWORD)(cbRam >> 32),
        (DWORD)(cbRam & 0xFFFFFFFF),
        L"Global\\VBoxHaywireMem"
    );

    if (!g_hSharedMemFile)
        return VERR_NO_MEMORY;

    g_pSharedMemBase = MapViewOfFile(
        g_hSharedMemFile,
        FILE_MAP_ALL_ACCESS,
        0, 0, cbRam
    );

    if (!g_pSharedMemBase) {
        CloseHandle(g_hSharedMemFile);
        return VERR_NO_MEMORY;
    }

    g_SharedMemSize = cbRam;
    g_SharedMemOffset = 0;

    return VINF_SUCCESS;
}
```

**File 2: `src/VBox/Runtime/r0drv/nt/memobj-r0drv-nt.cpp`**

Hook the allocation:
```cpp
// External reference to shared memory
extern "C" {
    extern void* g_pSharedMemBase;
    extern size_t g_SharedMemOffset;
    extern size_t g_SharedMemSize;
}

static void* AllocateFromSharedMem(size_t cb) {
    if (!g_pSharedMemBase)
        return NULL;  // Feature not enabled

    // Only intercept 2MB allocations (guest RAM chunks)
    if (cb != (2 * 1024 * 1024))
        return NULL;

    // Check if we have space
    if (g_SharedMemOffset + cb > g_SharedMemSize)
        return NULL;

    // Allocate from shared region
    void* pChunk = (char*)g_pSharedMemBase + g_SharedMemOffset;
    g_SharedMemOffset += cb;

    return pChunk;
}

// In the allocation function, add at the beginning:
void* ptr = AllocateFromSharedMem(cb);
if (ptr)
    return CreateMemObjForSharedChunk(ptr, cb);

// Fall through to normal MmAllocatePagesForMdl...
```

### Phase 3: Build (2-4 hours first time)

```cmd
cd C:\Users\jamie\vbox-src
configure.vbs --with-visual-studio=2022
cd out\win.amd64\release
kmk
```

### Phase 4: Test (2 hours)

1. Uninstall existing VirtualBox
2. Install your patched version
3. Start VM
4. Verify shared memory file created: `Global\VBoxHaywireMem`
5. Test Haywire can mmap and read it

### Phase 5: Integrate with Haywire (2 hours)

Add `VBoxConnection` class that opens the shared memory file.

---

## Total Timeline

**Source Patch Approach:**
- Day 1: Setup build environment, implement patch
- Day 2: Build, test, debug
- Day 3: Integrate with Haywire, polish

**Total: ~3 days of focused work**

---

## Alternative: Quick Win with Periodic Dumps

If you want something working TODAY:

```python
# vbox_live_bridge.py
import time
import subprocess

while True:
    # Dump memory
    subprocess.run([
        "VBoxManage", "debugvm", "Ubuntu-x86_64-Haywire",
        "dumpvmcore", "--filename", "C:\\temp\\vm-live.elf"
    ])

    # Parse to flat file
    subprocess.run(["python", "parse_vbox_dump.py",
                    "C:\\temp\\vm-live.elf",
                    "C:\\temp\\haywire-vbox-mem"])

    time.sleep(5)  # Refresh every 5 seconds
```

Then Haywire reads `C:\temp\haywire-vbox-mem`.

**Pros:** Works in 10 minutes
**Cons:** 5 second latency, high I/O

---

## Decision

Given you want to "keep working on VB", I recommend:

**Start with Quick Win dumps** (working today) → **Transition to Source Patch** (3 days for production quality)

This gives you:
1. Immediate validation that VirtualBox memory has what you need
2. Time to set up build environment properly
3. A working baseline while developing the patch

**Should I help you set up the quick dump-based bridge first, or dive straight into the source patch?**
