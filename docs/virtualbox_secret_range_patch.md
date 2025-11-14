# VirtualBox "Secret Range" Patch - Backup Plan for Windows

## Purpose

This is a **fallback option** if QEMU with WSL2+KVM proves difficult or unstable on Windows. The goal is to make VirtualBox behave like QEMU's memory-backend-file by redirecting chunk allocation to a shared memory region.

## When to Use This

**Try these first (in order):**
1. ✅ QEMU with WSL2+KVM on Windows 11 (recommended)
2. ✅ QEMU with HVF on Intel macOS
3. ✅ QEMU with KVM on Intel Linux

**Use VirtualBox patch only if:**
- WSL2+KVM is unstable or unavailable
- You need VirtualBox for other reasons (specific features, existing setup, etc.)
- You're doing research/experimentation with hypervisor internals

## The Core Idea

VirtualBox allocates guest RAM in **2MB chunks** using the kernel's page allocator. The "secret range" patch intercepts these allocations and redirects them to a pre-allocated mmap'd file.

**Key insight:** VirtualBox chunks contain **no intrusive metadata** - they're pure 2MB blocks of guest RAM!

## Memory Layout (VirtualBox)

### Current VirtualBox Architecture

```
GMMCHUNK metadata (stored separately in kernel heap):
┌─────────────────────────────┐
│ struct GMMCHUNK             │  ~4KB per chunk
│  ├─ pbMapping ───────┐      │  (pointer to actual RAM)
│  ├─ aPages[512]      │      │  (8 bytes × 512 pages = 4KB)
│  └─ ... metadata     │      │
└─────────────────────────────┘
                       │
                       ▼
Actual 2MB chunk (allocated via alloc_pages):
┌─────────────────────────────┐
│ Pure guest RAM (2MB)        │  No headers!
│ Exactly 2,097,152 bytes     │  No metadata!
└─────────────────────────────┘
```

### After "Secret Range" Patch

```
GMMCHUNK metadata (unchanged):
┌─────────────────────────────┐
│ struct GMMCHUNK             │
│  ├─ pbMapping ───────┐      │
│  └─ ...              │      │
└─────────────────────────────┘
                       │
                       ▼
Secret range (mmap'd file at /dev/shm/vbox-vm-mem):
┌─────────────────────────────┐
│ Chunk 0: 2MB guest RAM      │  Offset 0x00000000
│ Chunk 1: 2MB guest RAM      │  Offset 0x00200000
│ Chunk 2: 2MB guest RAM      │  Offset 0x00400000
│ ...                         │
│ Chunk N: 2MB guest RAM      │
└─────────────────────────────┘
         ▲
         │
    Haywire mmaps this same file!
    Sees identical layout to QEMU.
```

## Implementation Plan

### Step 1: Pre-allocate Shared Memory File

**File:** `src/VBox/VMM/VMMR3/PGM.cpp`

```c
#include <sys/mman.h>
#include <fcntl.h>

static void* g_pSharedMemBase = NULL;
static size_t g_SharedMemOffset = 0;
static size_t g_SharedMemSize = 0;
static int g_fdSharedMem = -1;

int pgmR3InitSharedMemoryBackend(PVM pVM)
{
    const size_t cbRam = pVM->pgm.s.cbRamSize;

    LogRel(("PGM: Initializing shared memory backend (%zu bytes)\n", cbRam));

    // Create shared memory file in tmpfs
    g_fdSharedMem = shm_open("/vbox-vm-mem", O_CREAT | O_RDWR, 0666);
    if (g_fdSharedMem < 0) {
        LogRel(("PGM: Failed to create shared memory file: %s\n", strerror(errno)));
        return VERR_NO_MEMORY;
    }

    // Set size
    if (ftruncate(g_fdSharedMem, cbRam) < 0) {
        LogRel(("PGM: Failed to set file size: %s\n", strerror(errno)));
        close(g_fdSharedMem);
        return VERR_NO_MEMORY;
    }

    // Try to map with huge pages (2MB pages = no fragmentation!)
    g_pSharedMemBase = mmap(NULL, cbRam,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_HUGETLB,
                            g_fdSharedMem, 0);

    if (g_pSharedMemBase == MAP_FAILED) {
        // Fallback: regular 4KB pages
        LogRel(("PGM: Huge pages unavailable, using regular pages\n"));
        g_pSharedMemBase = mmap(NULL, cbRam,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED,
                                g_fdSharedMem, 0);
    }

    if (g_pSharedMemBase == MAP_FAILED) {
        LogRel(("PGM: Failed to mmap shared memory: %s\n", strerror(errno)));
        close(g_fdSharedMem);
        return VERR_NO_MEMORY;
    }

    // Lock pages in memory (prevent swapping)
    if (mlock(g_pSharedMemBase, cbRam) < 0) {
        LogRel(("PGM: Warning: Failed to lock memory: %s\n", strerror(errno)));
        // Continue anyway - not fatal
    }

    g_SharedMemSize = cbRam;
    g_SharedMemOffset = 0;

    LogRel(("PGM: Shared memory backend ready at %p (%s)\n",
            g_pSharedMemBase,
            (madvise(g_pSharedMemBase, 0, MADV_HUGEPAGE) == 0) ?
                "hugepages" : "regular"));

    return VINF_SUCCESS;
}

void pgmR3CleanupSharedMemoryBackend(void)
{
    if (g_pSharedMemBase) {
        munlock(g_pSharedMemBase, g_SharedMemSize);
        munmap(g_pSharedMemBase, g_SharedMemSize);
        g_pSharedMemBase = NULL;
    }

    if (g_fdSharedMem >= 0) {
        close(g_fdSharedMem);
        shm_unlink("/vbox-vm-mem");
        g_fdSharedMem = -1;
    }
}
```

### Step 2: Redirect Chunk Allocation

**File:** `src/VBox/Runtime/r0drv/linux/memobj-r0drv-linux.c`

```c
// External reference to shared memory region
extern void* g_pSharedMemBase;
extern size_t g_SharedMemOffset;
extern size_t g_SharedMemSize;

/**
 * Allocate from shared memory region instead of kernel allocator.
 *
 * This is a simple bump allocator - chunks are allocated sequentially
 * from the pre-mapped region. Because VirtualBox chunks have no intrusive
 * metadata, they can be laid out contiguously in the file.
 */
static struct page* rtR0MemObjLinuxAllocPagesFromShared(size_t cPages)
{
    if (!g_pSharedMemBase)
        return NULL;  // Feature not enabled

    size_t cbNeeded = cPages * PAGE_SIZE;

    // Check if we have space left
    if (g_SharedMemOffset + cbNeeded > g_SharedMemSize)
        return NULL;  // Out of space, fall back to normal allocation

    // Allocate from shared region (bump allocator)
    void* pChunk = (char*)g_pSharedMemBase + g_SharedMemOffset;
    g_SharedMemOffset += cbNeeded;

    // Convert virtual address to page struct
    // This works because the mmap'd region has actual pages backing it
    struct page* pPage = virt_to_page(pChunk);

    printk(KERN_INFO "VBox: Allocated %zu pages from shared memory at offset 0x%zx\n",
           cPages, g_SharedMemOffset - cbNeeded);

    return pPage;
}

// In rtR0MemObjLinuxAllocPages(), at the very beginning:

// Try shared memory allocation first
paPages = rtR0MemObjLinuxAllocPagesFromShared(cPages);
if (paPages)
    goto allocation_success;  // Jump to existing success handling code

// Fall through to existing alloc_pages() code if shared memory unavailable...
```

### Step 3: Enable via Configuration

**File:** `src/VBox/Main/src-server/MachineImpl.cpp` (optional)

Add VMX configuration option:

```xml
<ExtraData>
    <ExtraDataItem name="VBoxInternal/PGM/SharedMemoryBackend" value="1"/>
</ExtraData>
```

Or just enable it always for development builds.

## Chunk Count by VM Size

| VM RAM | Chunks (2MB each) | Metadata Size |
|--------|------------------|---------------|
| 2GB | 1,024 | ~4.1 MB |
| 4GB | 2,048 | ~8.2 MB |
| 8GB | 4,096 | ~16.4 MB |
| 16GB | 8,192 | ~32.8 MB |

## Performance Considerations

### Best Case: Huge Pages Enabled

If the system has huge pages (2MB) enabled:
- `MAP_HUGETLB` succeeds
- Each VirtualBox chunk maps to exactly one huge page
- **Zero fragmentation**
- **Optimal TLB performance**

```bash
# Enable huge pages on Linux:
echo 2048 | sudo tee /proc/sys/vm/nr_hugepages
# (2048 × 2MB = 4GB of huge pages)
```

### Fallback: Regular 4KB Pages

If huge pages unavailable:
- mmap uses regular 4KB pages
- VirtualBox chunks span 512 × 4KB pages each
- Slightly more TLB pressure
- Still perfectly functional

### Zero Copy Performance

**Key advantage over background sync approach:**

Background sync (NOT used):
```
Guest writes → VirtualBox RAM → memcpy to file → Haywire reads
                                 ^^^^^^ Overhead!
```

Secret range (used):
```
Guest writes → Shared mmap → Haywire reads
               (same physical pages, zero copy!)
```

## Code Complexity

**Lines of code:** ~150 lines total
- `pgmR3InitSharedMemoryBackend()`: ~80 lines
- `rtR0MemObjLinuxAllocPagesFromShared()`: ~40 lines
- Cleanup and config: ~30 lines

**Files modified:** 3 files
- `src/VBox/VMM/VMMR3/PGM.cpp`
- `src/VBox/Runtime/r0drv/linux/memobj-r0drv-linux.c`
- `src/VBox/Main/src-server/MachineImpl.cpp` (optional config)

**Effort estimate:** 2-3 weeks
- Implementation: 1 week
- Testing: 1 week
- Debugging/polish: 1 week

## Limitations

1. **Custom fork** - Must maintain patch across VirtualBox updates
2. **Linux-only initially** - Would need Windows/macOS variants
3. **No upstream** - Unlikely to be accepted by Oracle (niche use case)
4. **Debugging complexity** - Kernel-level memory management code

## Comparison to Alternatives

| Solution | Pros | Cons |
|----------|------|------|
| **QEMU/WSL2+KVM** | Already works, supported, identical across platforms | Requires WSL2 setup |
| **QEMU/WHPX** | Native Windows | Buggy with Linux guests (MSI errors) |
| **VirtualBox patch** | Clean design, zero copy, no WHPX bugs | Custom fork to maintain |

## Testing Plan

### Phase 1: Basic Functionality
- [ ] Patch compiles without errors
- [ ] Shared memory file created at `/dev/shm/vbox-vm-mem`
- [ ] VM boots successfully
- [ ] Guest OS runs normally (no crashes)

### Phase 2: Memory Verification
- [ ] Verify chunks allocated from shared region
- [ ] Check file size matches VM RAM size
- [ ] Confirm no gaps in memory layout
- [ ] Test with different VM sizes (2GB, 4GB, 8GB)

### Phase 3: Haywire Integration
- [ ] Haywire can mmap `/dev/shm/vbox-vm-mem`
- [ ] Memory contents match guest RAM
- [ ] Live updates visible (write to guest, see in Haywire)
- [ ] Kernel discovery works correctly
- [ ] Process introspection works

### Phase 4: Performance
- [ ] Benchmark VM performance vs. unpatched VirtualBox
- [ ] Test with/without huge pages
- [ ] Memory bandwidth tests
- [ ] Stability over extended runtime

## When to Implement This

**Implement now if:**
- You're experimenting with hypervisor internals (research/learning)
- You need VirtualBox specifically (existing infrastructure)
- You want a fallback option ready

**Wait if:**
- WSL2+KVM works fine (likely scenario)
- You don't want to maintain a custom VirtualBox fork
- You prefer using supported software

## Conclusion

The "secret range" patch is a **technically elegant solution** that would make VirtualBox work identically to QEMU's memory-backend-file. The key insight is that VirtualBox chunks are pure guest RAM with no intrusive metadata, making them perfect for sequential layout in a shared file.

**Recommended approach:**
1. Start with QEMU + WSL2+KVM on Windows (easiest)
2. Keep this VirtualBox patch design as backup plan
3. Implement VirtualBox patch only if needed

The fact that VirtualBox chunks have no headers means this would be a clean, zero-copy solution if you ever need it!
