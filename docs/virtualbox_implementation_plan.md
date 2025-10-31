# VirtualBox Secret Range Implementation Plan

## Goal

Modify VirtualBox to allocate guest RAM from a shared memory file instead of kernel allocator, enabling live memory introspection identical to QEMU's memory-backend-file approach.

## Why This Works

VirtualBox allocates guest RAM in **2MB chunks** (GMM_CHUNK_SIZE) with:
- Metadata stored separately in GMMCHUNK structures (~4KB each)
- Actual chunks are **pure guest RAM** - no headers, no pointers, no intrusive data
- Perfect for sequential layout in a shared file

## Implementation Phases

### Phase 1: VirtualBox Setup (Both Platforms)

#### macOS Setup
```bash
# Download VirtualBox from https://www.virtualbox.org/
# Current version: 7.0.x

# Install VirtualBox
# Download source code for patching:
cd ~/Downloads
wget https://download.virtualbox.org/virtualbox/7.0.14/VirtualBox-7.0.14.tar.bz2
tar xjf VirtualBox-7.0.14.tar.bz2
cd VirtualBox-7.0.14

# Install build dependencies
brew install gcc make kernel-headers
```

#### Windows Setup
```powershell
# Download VirtualBox from https://www.virtualbox.org/
# Install normally

# For building from source:
# Download Visual Studio 2019 or later
# Download Windows SDK
# Download VirtualBox source (same version as above)
```

### Phase 2: Locate Key Source Files

The files we need to modify are in the VirtualBox source tree:

```
VirtualBox-7.0.14/
├── src/VBox/VMM/VMMR3/
│   └── PGM.cpp                    # Physical memory manager (userspace)
├── src/VBox/Runtime/r0drv/
│   ├── linux/
│   │   └── memobj-r0drv-linux.c   # Linux kernel memory allocation
│   ├── darwin/
│   │   └── memobj-r0drv-darwin.cpp # macOS kernel memory allocation
│   └── nt/
│       └── memobj-r0drv-nt.cpp     # Windows kernel memory allocation
└── src/VBox/Main/src-server/
    └── MachineImpl.cpp             # VM configuration (optional)
```

### Phase 3: Shared Memory Backend (Userspace)

**File: `src/VBox/VMM/VMMR3/PGM.cpp`**

Add global state for shared memory region:

```cpp
// Near top of file, after includes
#ifdef RT_OS_POSIX
# include <sys/mman.h>
# include <fcntl.h>
#endif

// Global shared memory state
static void*   g_pSharedMemBase = NULL;
static size_t  g_SharedMemOffset = 0;
static size_t  g_SharedMemSize = 0;
static RTFILE  g_hSharedMemFile = NIL_RTFILE;

#ifdef RT_OS_POSIX
static int     g_fdSharedMem = -1;
#endif
```

Add initialization function (call during VM startup):

```cpp
/**
 * Initialize shared memory backend for guest RAM.
 *
 * Creates a shared memory file that kernel allocator will use
 * instead of normal page allocation. This makes all guest RAM
 * visible to external tools like Haywire.
 */
static int pgmR3InitSharedMemoryBackend(PVM pVM)
{
    const size_t cbRam = pVM->pgm.s.cbRamSize;

    LogRel(("PGM: Initializing shared memory backend (%zu bytes)\n", cbRam));

#ifdef RT_OS_POSIX
    // POSIX (Linux/macOS): Use shm_open
    const char *pszShmName = "/vbox-vm-mem";

    // Unlink any stale file from previous run
    shm_unlink(pszShmName);

    g_fdSharedMem = shm_open(pszShmName, O_CREAT | O_RDWR, 0666);
    if (g_fdSharedMem < 0)
    {
        LogRel(("PGM: Failed to create shared memory: %s\n", strerror(errno)));
        return VERR_NO_MEMORY;
    }

    // Set size
    if (ftruncate(g_fdSharedMem, cbRam) < 0)
    {
        LogRel(("PGM: Failed to set size: %s\n", strerror(errno)));
        close(g_fdSharedMem);
        shm_unlink(pszShmName);
        return VERR_NO_MEMORY;
    }

    // Try huge pages first (2MB pages = perfect alignment!)
    g_pSharedMemBase = mmap(NULL, cbRam,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_HUGETLB,
                            g_fdSharedMem, 0);

    if (g_pSharedMemBase == MAP_FAILED)
    {
        // Fallback: regular 4KB pages
        LogRel(("PGM: Huge pages unavailable, using regular pages\n"));
        g_pSharedMemBase = mmap(NULL, cbRam,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED,
                                g_fdSharedMem, 0);
    }

    if (g_pSharedMemBase == MAP_FAILED)
    {
        LogRel(("PGM: Failed to mmap: %s\n", strerror(errno)));
        close(g_fdSharedMem);
        shm_unlink(pszShmName);
        return VERR_NO_MEMORY;
    }

    // Lock pages in memory (prevent swapping)
    if (mlock(g_pSharedMemBase, cbRam) < 0)
    {
        LogRel(("PGM: Warning: Failed to lock memory: %s\n", strerror(errno)));
        // Continue anyway - not fatal
    }

#elif defined(RT_OS_WINDOWS)
    // Windows: Use CreateFileMapping with pagefile backing
    HANDLE hMapping = CreateFileMapping(
        INVALID_HANDLE_VALUE,  // Use pagefile
        NULL,                  // Default security
        PAGE_READWRITE,        // Read/write access
        (DWORD)(cbRam >> 32),  // High-order DWORD of size
        (DWORD)(cbRam & 0xFFFFFFFF),  // Low-order DWORD
        L"Global\\vbox-vm-mem");  // Name (accessible from other processes)

    if (hMapping == NULL)
    {
        LogRel(("PGM: Failed to create mapping: %d\n", GetLastError()));
        return VERR_NO_MEMORY;
    }

    g_pSharedMemBase = MapViewOfFile(hMapping,
                                     FILE_MAP_ALL_ACCESS,
                                     0, 0, cbRam);

    if (g_pSharedMemBase == NULL)
    {
        LogRel(("PGM: Failed to map view: %d\n", GetLastError()));
        CloseHandle(hMapping);
        return VERR_NO_MEMORY;
    }

    g_hSharedMemFile = (RTFILE)hMapping;  // Store for cleanup
#endif

    g_SharedMemSize = cbRam;
    g_SharedMemOffset = 0;

    LogRel(("PGM: Shared memory backend ready at %p\n", g_pSharedMemBase));

    return VINF_SUCCESS;
}

/**
 * Cleanup shared memory backend.
 */
static void pgmR3CleanupSharedMemoryBackend(void)
{
    if (g_pSharedMemBase == NULL)
        return;

#ifdef RT_OS_POSIX
    munlock(g_pSharedMemBase, g_SharedMemSize);
    munmap(g_pSharedMemBase, g_SharedMemSize);

    if (g_fdSharedMem >= 0)
    {
        close(g_fdSharedMem);
        shm_unlink("/vbox-vm-mem");
        g_fdSharedMem = -1;
    }
#elif defined(RT_OS_WINDOWS)
    UnmapViewOfFile(g_pSharedMemBase);
    if (g_hSharedMemFile != NIL_RTFILE)
    {
        CloseHandle((HANDLE)g_hSharedMemFile);
        g_hSharedMemFile = NIL_RTFILE;
    }
#endif

    g_pSharedMemBase = NULL;
    g_SharedMemSize = 0;
    g_SharedMemOffset = 0;
}
```

### Phase 4: Kernel Allocator Redirect

#### Linux: `src/VBox/Runtime/r0drv/linux/memobj-r0drv-linux.c`

```c
// External reference to shared memory region (declared in PGM.cpp)
extern void*  g_pSharedMemBase;
extern size_t g_SharedMemOffset;
extern size_t g_SharedMemSize;

/**
 * Allocate pages from shared memory region instead of kernel.
 *
 * This is a simple bump allocator - chunks allocated sequentially.
 * Works because VirtualBox chunks have no intrusive metadata!
 */
static struct page* rtR0MemObjLinuxAllocPagesFromShared(size_t cPages)
{
    if (!g_pSharedMemBase)
        return NULL;  // Feature not enabled

    size_t cbNeeded = cPages * PAGE_SIZE;

    // VirtualBox always allocates 2MB chunks (512 pages)
    // Verify this assumption
    if (cPages != 512)
    {
        printk(KERN_WARNING "VBox: Unexpected allocation size: %zu pages\n", cPages);
        return NULL;
    }

    // Check space available
    if (g_SharedMemOffset + cbNeeded > g_SharedMemSize)
    {
        printk(KERN_WARNING "VBox: Shared memory exhausted at offset 0x%zx\n",
               g_SharedMemOffset);
        return NULL;
    }

    // Allocate from shared region (bump allocator)
    void* pChunk = (char*)g_pSharedMemBase + g_SharedMemOffset;
    size_t offset = g_SharedMemOffset;
    g_SharedMemOffset += cbNeeded;

    // Convert virtual address to page struct
    struct page* pPage = virt_to_page(pChunk);

    printk(KERN_INFO "VBox: Allocated %zu pages from shared memory at offset 0x%zx\n",
           cPages, offset);

    return pPage;
}

// In rtR0MemObjLinuxAllocPages(), at the very beginning:
// (Find the function that calls alloc_pages)

DECLHIDDEN(int) rtR0MemObjLinuxAllocPages(...)
{
    // ... existing variable declarations ...

    // NEW: Try shared memory allocation first
    paPages = rtR0MemObjLinuxAllocPagesFromShared(cPages);
    if (paPages)
    {
        // Success! Jump to existing success handling code
        goto allocation_success;
    }

    // Fall through to existing alloc_pages() code if shared memory unavailable
    // ... rest of existing function ...
}
```

#### macOS: `src/VBox/Runtime/r0drv/darwin/memobj-r0drv-darwin.cpp`

Similar approach, but using macOS kernel APIs:

```cpp
extern void*  g_pSharedMemBase;
extern size_t g_SharedMemOffset;
extern size_t g_SharedMemSize;

static void* rtR0MemObjDarwinAllocFromShared(size_t cb)
{
    if (!g_pSharedMemBase)
        return NULL;

    if (cb != _2M)  // VirtualBox uses 2MB chunks
    {
        printf("VBox: Unexpected allocation size: %zu\n", cb);
        return NULL;
    }

    if (g_SharedMemOffset + cb > g_SharedMemSize)
    {
        printf("VBox: Shared memory exhausted\n");
        return NULL;
    }

    void* pChunk = (char*)g_pSharedMemBase + g_SharedMemOffset;
    g_SharedMemOffset += cb;

    printf("VBox: Allocated chunk from shared memory at offset 0x%zx\n",
           g_SharedMemOffset - cb);

    return pChunk;
}
```

#### Windows: `src/VBox/Runtime/r0drv/nt/memobj-r0drv-nt.cpp`

Windows kernel version:

```cpp
extern void*  g_pSharedMemBase;
extern size_t g_SharedMemOffset;
extern size_t g_SharedMemSize;

static PMDL rtR0MemObjNtAllocFromShared(SIZE_T cb)
{
    if (!g_pSharedMemBase)
        return NULL;

    if (cb != _2M)
    {
        DbgPrint("VBox: Unexpected allocation size: %zu\n", cb);
        return NULL;
    }

    if (g_SharedMemOffset + cb > g_SharedMemSize)
    {
        DbgPrint("VBox: Shared memory exhausted\n");
        return NULL;
    }

    void* pChunk = (PUCHAR)g_pSharedMemBase + g_SharedMemOffset;
    g_SharedMemOffset += cb;

    // Create MDL for this memory region
    PMDL pMdl = IoAllocateMdl(pChunk, (ULONG)cb, FALSE, FALSE, NULL);
    if (pMdl)
        MmBuildMdlForNonPagedPool(pMdl);

    return pMdl;
}
```

### Phase 5: Build VirtualBox

#### Linux Build
```bash
cd VirtualBox-7.0.14
./configure --disable-hardening
source env.sh
kmk

# Install kernel modules
sudo make install
```

#### macOS Build
```bash
cd VirtualBox-7.0.14
./configure --disable-hardening
source env.sh
kmk

# Sign and load kernel extensions (requires disabling SIP temporarily)
```

#### Windows Build
```cmd
cd VirtualBox-7.0.14
cscript configure.vbs --with-MinGW
env.bat
kmk
```

### Phase 6: Testing

#### Create Test VM
```bash
# Linux/macOS
VBoxManage createvm --name "Ubuntu-Test" --ostype Ubuntu_64 --register
VBoxManage modifyvm "Ubuntu-Test" --memory 4096 --cpus 2
VBoxManage createhd --filename Ubuntu-Test.vdi --size 20480
VBoxManage storagectl "Ubuntu-Test" --name SATA --add sata
VBoxManage storageattach "Ubuntu-Test" --storagectl SATA --port 0 \
    --type hdd --medium Ubuntu-Test.vdi
VBoxManage storageattach "Ubuntu-Test" --storagectl SATA --port 1 \
    --type dvddrive --medium ubuntu-24.04-desktop-amd64.iso
```

#### Verify Shared Memory
```bash
# Start VM
VBoxManage startvm "Ubuntu-Test"

# Check shared memory file exists (Linux/macOS)
ls -lh /dev/shm/vbox-vm-mem  # Should be 4GB

# Check VirtualBox logs
tail -f ~/VirtualBox\ VMs/Ubuntu-Test/Logs/VBox.log | grep "Shared memory"
# Should see: "PGM: Shared memory backend ready at 0x..."
```

#### Test with Haywire
```bash
cd ~/haywire/build

# Create symlink to VirtualBox shared memory
ln -sf /dev/shm/vbox-vm-mem /tmp/haywire-vm-mem

# Run Haywire
./haywire
```

### Phase 7: Validation Checklist

- [ ] Shared memory file created at correct path
- [ ] File size matches VM RAM size (4GB = 4,294,967,296 bytes)
- [ ] VirtualBox logs show "Allocated from shared memory" messages
- [ ] Chunk count matches expected (4GB ÷ 2MB = 2,048 chunks)
- [ ] VM boots normally (no crashes)
- [ ] Guest OS runs without issues
- [ ] Haywire can open the memory file
- [ ] Haywire discovers swapper_pgd correctly
- [ ] Process discovery works
- [ ] Memory contents match guest RAM

## Platform-Specific Memory Paths

### Linux
```
/dev/shm/vbox-vm-mem  (4GB for 4GB VM)
```

### macOS
```
/tmp/vbox-vm-mem  (shm_open creates in /var/folders on macOS)
```

### Windows
```
Global\vbox-vm-mem  (accessed via MapViewOfFile)
```

For Haywire on Windows, you'd need to:
```cpp
HANDLE hMapping = OpenFileMapping(FILE_MAP_READ, FALSE, L"Global\\vbox-vm-mem");
void* pMem = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
```

## Debugging Tips

### Enable VirtualBox Logging
```bash
# Set environment variable before starting VM
export VBOX_LOG=+pgm.e.l.f
export VBOX_LOG_DEST=file=/tmp/vbox-debug.log
VBoxManage startvm "Ubuntu-Test"

# Check logs
tail -f /tmp/vbox-debug.log
```

### Verify Chunk Allocation
```bash
# Count "Allocated from shared memory" messages
grep "Allocated from shared memory" ~/VirtualBox\ VMs/Ubuntu-Test/Logs/VBox.log | wc -l
# Should match expected chunk count (e.g., 2048 for 4GB VM)
```

### Check Memory Layout
```bash
# Dump first chunk (2MB) to verify it's guest RAM
dd if=/dev/shm/vbox-vm-mem of=/tmp/chunk0.bin bs=2M count=1

# Should contain guest boot code, not zeros or random data
hexdump -C /tmp/chunk0.bin | head -20
```

## Known Issues and Solutions

### Issue: "virt_to_page() returned NULL"
**Cause**: Shared memory region not properly mapped in kernel space
**Solution**: Ensure mmap() succeeded and memory is locked with mlock()

### Issue: VM crashes on boot
**Cause**: Shared memory exhausted (offset exceeded size)
**Solution**: Increase shared memory size or check for memory leaks

### Issue: Chunks not allocated from shared memory
**Cause**: Existing allocation path took priority
**Solution**: Ensure shared memory check is FIRST in allocation function

### Issue: Permission denied accessing /dev/shm/vbox-vm-mem
**Cause**: File created with restrictive permissions
**Solution**: Use mode 0666 in shm_open(), or chmod after creation

## Performance Considerations

### Huge Pages (Best Case)
If huge pages enabled (Linux: `echo 2048 > /proc/sys/vm/nr_hugepages`):
- Each VirtualBox chunk = exactly one 2MB huge page
- Zero fragmentation
- Optimal TLB performance

### Regular Pages (Fallback)
If huge pages unavailable:
- mmap uses regular 4KB pages
- VirtualBox chunks span 512 × 4KB pages each
- Slightly more TLB pressure
- Still perfectly functional

## Comparison to Alternatives

| Solution | Pros | Cons |
|----------|------|------|
| **QEMU + memory-backend-file** | Already works, supported | Requires QEMU |
| **VirtualBox + secret range** | Works with existing VBox VMs | Custom fork to maintain |
| **VMware** | Popular, mature | No live memory access |
| **Hyper-V** | Native on Windows | Complex memory access |

## Next Steps

1. Download VirtualBox source matching your installed version
2. Apply patches to PGM.cpp and memobj-r0drv-*.c
3. Build VirtualBox from source
4. Test with simple Ubuntu VM
5. Validate with Haywire
6. Document any issues found
7. Consider submitting upstream (though unlikely to be accepted)

## Maintenance

This is a **custom fork** that must be maintained:
- Track VirtualBox updates
- Reapply patches to new versions
- Test with each VirtualBox release
- Keep fork synchronized with upstream

**Effort estimate**: 1-2 days per VirtualBox release

## Alternative: Use QEMU

If VirtualBox patching becomes too burdensome:
- QEMU works excellently on Intel with proper acceleration
- memory-backend-file is a supported feature
- No custom patches needed
- Identical workflow across all platforms

The VirtualBox patch is elegant and educational, but QEMU is the pragmatic choice for production use.
