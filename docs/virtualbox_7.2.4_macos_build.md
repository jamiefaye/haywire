# Building VirtualBox 7.2.4 with Secret Range Patch on macOS ARM64

## Overview

This guide walks through building VirtualBox 7.2.4 from source on macOS ARM64 with the "secret range" patch to enable live memory introspection for Haywire.

**Goal**: Redirect VirtualBox's 2MB chunk allocator to a shared memory file at `/tmp/vbox-vm-mem` that Haywire can read.

## Prerequisites

### Install Xcode Command Line Tools
```bash
xcode-select --install
```

### Install Build Dependencies
```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install gcc make autoconf automake libtool pkg-config
brew install openssl libvpx opus libvorbis SDL2
brew install qt@5  # VirtualBox uses Qt5, not Qt6
```

### Download VirtualBox Source

```bash
cd ~/Downloads
curl -LO https://download.virtualbox.org/virtualbox/7.2.4/VirtualBox-7.2.4.tar.bz2
curl -LO https://download.virtualbox.org/virtualbox/7.2.4/UserManual.pdf  # Optional

# Verify checksum (optional but recommended)
curl -LO https://download.virtualbox.org/virtualbox/7.2.4/SHA256SUMS
shasum -a 256 -c SHA256SUMS 2>&1 | grep VirtualBox-7.2.4.tar.bz2

# Extract
tar xjf VirtualBox-7.2.4.tar.bz2
cd VirtualBox-7.2.4
```

## Implementation Steps

### Step 1: Apply Shared Memory Backend Patch

**File: `src/VBox/VMM/VMMR3/PGM.cpp`**

First, let me create a patch directory:

```bash
mkdir -p ~/haywire/patches
cd ~/haywire/patches
```

Create the patch file `vbox-7.2.4-shared-memory-backend.patch`:

```patch
diff --git a/src/VBox/VMM/VMMR3/PGM.cpp b/src/VBox/VMM/VMMR3/PGM.cpp
index 1234567..abcdefg 100644
--- a/src/VBox/VMM/VMMR3/PGM.cpp
+++ b/src/VBox/VMM/VMMR3/PGM.cpp
@@ -40,6 +40,10 @@
 #include <iprt/thread.h>
 #include <iprt/string.h>

+#ifdef RT_OS_POSIX
+# include <sys/mman.h>
+# include <fcntl.h>
+#endif

 /*********************************************************************************************************************************
 *   Defined Constants And Macros                                                                                                 *
@@ -60,6 +64,50 @@
 *   Global Variables                                                                                                              *
 *********************************************************************************************************************************/

+/*********************************************************************************************************************************
+*   Shared Memory Backend for Live Introspection                                                                                 *
+*********************************************************************************************************************************/
+
+/** Shared memory base pointer (mmap'd region). */
+static void*   g_pSharedMemBase = NULL;
+/** Current allocation offset in shared memory. */
+static size_t  g_SharedMemOffset = 0;
+/** Total size of shared memory region. */
+static size_t  g_SharedMemSize = 0;
+
+#ifdef RT_OS_DARWIN
+/** File descriptor for shared memory (POSIX). */
+static int     g_fdSharedMem = -1;
+
+/**
+ * Initialize shared memory backend for guest RAM.
+ *
+ * Creates a shared memory file that the kernel allocator will use
+ * instead of normal page allocation. Enables live memory introspection.
+ *
+ * @returns VBox status code.
+ * @param   pVM     The cross context VM structure.
+ */
+static int pgmR3InitSharedMemoryBackend(PVM pVM)
+{
+    const size_t cbRam = pVM->pgm.s.cbRamSize;
+
+    LogRel(("PGM: Initializing shared memory backend (%zu bytes)\n", cbRam));
+
+    // POSIX (macOS): Use shared memory via mmap
+    const char *pszShmPath = "/tmp/vbox-vm-mem";
+
+    // Remove any stale file
+    unlink(pszShmPath);
+
+    g_fdSharedMem = open(pszShmPath, O_CREAT | O_RDWR, 0666);
+    if (g_fdSharedMem < 0)
+    {
+        LogRel(("PGM: Failed to create shared memory: %s\n", strerror(errno)));
+        return VERR_NO_MEMORY;
+    }
+
+    // Set size
+    if (ftruncate(g_fdSharedMem, cbRam) < 0)
+    {
+        LogRel(("PGM: Failed to set size: %s\n", strerror(errno)));
+        close(g_fdSharedMem);
+        unlink(pszShmPath);
+        return VERR_NO_MEMORY;
+    }
+
+    // Map with MAP_SHARED for live access
+    g_pSharedMemBase = mmap(NULL, cbRam,
+                            PROT_READ | PROT_WRITE,
+                            MAP_SHARED,
+                            g_fdSharedMem, 0);
+
+    if (g_pSharedMemBase == MAP_FAILED)
+    {
+        LogRel(("PGM: Failed to mmap: %s\n", strerror(errno)));
+        close(g_fdSharedMem);
+        unlink(pszShmPath);
+        return VERR_NO_MEMORY;
+    }
+
+    g_SharedMemSize = cbRam;
+    g_SharedMemOffset = 0;
+
+    LogRel(("PGM: Shared memory backend ready at %p (%s)\n",
+            g_pSharedMemBase, pszShmPath));
+
+    return VINF_SUCCESS;
+}
+
+/**
+ * Cleanup shared memory backend.
+ */
+static void pgmR3CleanupSharedMemoryBackend(void)
+{
+    if (g_pSharedMemBase == NULL)
+        return;
+
+    munmap(g_pSharedMemBase, g_SharedMemSize);
+
+    if (g_fdSharedMem >= 0)
+    {
+        close(g_fdSharedMem);
+        unlink("/tmp/vbox-vm-mem");
+        g_fdSharedMem = -1;
+    }
+
+    g_pSharedMemBase = NULL;
+    g_SharedMemSize = 0;
+    g_SharedMemOffset = 0;
+}
+#endif /* RT_OS_DARWIN */
+
 /**
  * Interface that PDMR3Term() and pgmR3HandlerPhysicalTypeInit() uses to
```

Now let's find the initialization function in PGM.cpp where we need to call our new function. We need to search for the VM initialization code:

```bash
cd ~/Downloads/VirtualBox-7.2.4
grep -n "pgmR3InitFinalize\|PGMR3Init" src/VBox/VMM/VMMR3/PGM.cpp | head -20
```

### Step 2: Apply Darwin Kernel Allocator Patch

**File: `src/VBox/Runtime/r0drv/darwin/memobj-r0drv-darwin.cpp`**

Create patch file `vbox-7.2.4-darwin-allocator.patch`:

```patch
diff --git a/src/VBox/Runtime/r0drv/darwin/memobj-r0drv-darwin.cpp b/src/VBox/Runtime/r0drv/darwin/memobj-r0drv-darwin.cpp
index 1234567..abcdefg 100644
--- a/src/VBox/Runtime/r0drv/darwin/memobj-r0drv-darwin.cpp
+++ b/src/VBox/Runtime/r0drv/darwin/memobj-r0drv-darwin.cpp
@@ -40,6 +40,13 @@
 *   Global Variables                                                                                                              *
 *********************************************************************************************************************************/

+/*********************************************************************************************************************************
+*   Shared Memory Backend (extern from PGM.cpp)                                                                                  *
+*********************************************************************************************************************************/
+extern void*  g_pSharedMemBase;
+extern size_t g_SharedMemOffset;
+extern size_t g_SharedMemSize;
+
+/**
+ * Allocate from shared memory region instead of kernel allocator.
+ *
+ * @returns Pointer to allocated memory, or NULL if shared memory unavailable.
+ * @param   cb      Size to allocate (must be 2MB for VirtualBox chunks).
+ */
+static void* rtR0MemObjDarwinAllocFromShared(size_t cb)
+{
+    // Check if shared memory is enabled
+    if (!g_pSharedMemBase)
+        return NULL;
+
+    // VirtualBox allocates in 2MB chunks (GMM_CHUNK_SIZE)
+    const size_t cbChunk = 2 * 1024 * 1024;
+    if (cb != cbChunk)
+    {
+        printf("VBox: Unexpected allocation size: %zu (expected %zu)\n", cb, cbChunk);
+        return NULL;
+    }
+
+    // Check space available
+    if (g_SharedMemOffset + cb > g_SharedMemSize)
+    {
+        printf("VBox: Shared memory exhausted at offset 0x%zx\n", g_SharedMemOffset);
+        return NULL;
+    }
+
+    // Allocate from shared region (bump allocator)
+    void* pChunk = (char*)g_pSharedMemBase + g_SharedMemOffset;
+    size_t offset = g_SharedMemOffset;
+    g_SharedMemOffset += cb;
+
+    printf("VBox: Allocated chunk from shared memory at offset 0x%zx\n", offset);
+
+    return pChunk;
+}
```

### Step 3: Configure VirtualBox Build

```bash
cd ~/Downloads/VirtualBox-7.2.4

# Configure for macOS ARM64
./configure \
    --disable-hardening \
    --disable-docs \
    --disable-java

# This creates LocalConfig.kmk
```

**Expected output:**
```
Checking for environment: Determined build machine: darwin.arm64, target machine: darwin.arm64
...
Successfully generated ...
```

### Step 4: Build VirtualBox

```bash
# Source the build environment
source ./env.sh

# Build (this will take 30-60 minutes)
kmk

# If kmk is not found, use:
# /Applications/VirtualBox.app/Contents/MacOS/kmk
```

**Note**: Building on macOS ARM64 may have issues since VirtualBox primarily targets x86_64. You may need to:
1. Build only the kernel modules we're patching
2. Use the pre-built binaries for other components

### Step 5: Install Modified Kernel Extension

```bash
# Unload existing VirtualBox kernel extensions
sudo kextunload -b org.virtualbox.kext.VBoxDrv
sudo kextunload -b org.virtualbox.kext.VBoxNetFlt
sudo kextunload -b org.virtualbox.kext.VBoxNetAdp

# Load modified kernel extensions
cd ~/Downloads/VirtualBox-7.2.4/out/darwin.arm64/release/dist
sudo kextload VirtualBox.kext

# Verify loaded
kextstat | grep -i vbox
```

### Step 6: Test with Ubuntu VM

```bash
# Create test VM
VBoxManage createvm --name "Ubuntu-Test-Haywire" --ostype Ubuntu_64 --register

# Configure VM
VBoxManage modifyvm "Ubuntu-Test-Haywire" \
    --memory 4096 \
    --cpus 2 \
    --vram 128 \
    --graphicscontroller vmsvga

# Create virtual disk
VBoxManage createhd \
    --filename ~/VirtualBox\ VMs/Ubuntu-Test-Haywire/Ubuntu-Test-Haywire.vdi \
    --size 20480

# Add storage controller
VBoxManage storagectl "Ubuntu-Test-Haywire" --name SATA --add sata --controller IntelAhci

# Attach disk
VBoxManage storageattach "Ubuntu-Test-Haywire" \
    --storagectl SATA \
    --port 0 \
    --device 0 \
    --type hdd \
    --medium ~/VirtualBox\ VMs/Ubuntu-Test-Haywire/Ubuntu-Test-Haywire.vdi

# Attach Ubuntu ISO (download first from ubuntu.com)
VBoxManage storageattach "Ubuntu-Test-Haywire" \
    --storagectl SATA \
    --port 1 \
    --device 0 \
    --type dvddrive \
    --medium ~/Downloads/ubuntu-24.04-desktop-arm64.iso

# Start VM
VBoxManage startvm "Ubuntu-Test-Haywire" --type gui
```

### Step 7: Verify Shared Memory

```bash
# Start the VM
VBoxManage startvm "Ubuntu-Test-Haywire"

# Check if shared memory file was created
ls -lh /tmp/vbox-vm-mem

# Should show 4GB file:
# -rw-r--r--  1 jamie  wheel   4.0G Oct 31 10:00 /tmp/vbox-vm-mem

# Check VirtualBox logs
tail -f ~/Library/Logs/VirtualBox/VBox.log | grep "Shared memory"
```

**Expected log output:**
```
PGM: Initializing shared memory backend (4294967296 bytes)
PGM: Shared memory backend ready at 0x... (/tmp/vbox-vm-mem)
VBox: Allocated chunk from shared memory at offset 0x0
VBox: Allocated chunk from shared memory at offset 0x200000
...
```

### Step 8: Test with Haywire

```bash
cd ~/haywire/build

# Haywire should see the memory file
./haywire

# In Haywire:
# 1. Click "Select Memory File"
# 2. Navigate to /tmp/vbox-vm-mem
# 3. Should show memory visualization
# 4. Click "Select Process" to test kernel discovery
```

## Troubleshooting

### Issue: "Operation not permitted" loading kext

**macOS System Integrity Protection (SIP) blocks unsigned kexts.**

**Solution**: Either:
1. Sign the kext with a valid Developer ID certificate
2. Disable SIP temporarily (not recommended):
   ```bash
   # Reboot into Recovery Mode (hold Cmd+R during boot)
   # In Terminal:
   csrutil disable
   # Reboot normally
   ```

### Issue: Build fails with "unknown target"

**VirtualBox 7.2.4 may not fully support macOS ARM64 builds.**

**Workaround**: Use the installed binaries and only rebuild the kernel extension:
```bash
cd ~/Downloads/VirtualBox-7.2.4
kmk BUILD_TARGET_ARCH=arm64 VBOX_ONLY_EXTPACKS=
```

### Issue: Shared memory file not created

**Check VirtualBox logs:**
```bash
cat ~/Library/Logs/VirtualBox/VBox.log | grep -i "shared memory"
```

**Possible causes:**
- Patch not applied correctly
- Initialization function not called during VM startup
- Insufficient permissions

### Issue: VM crashes on boot

**Shared memory allocation failed.**

**Debug steps:**
1. Check log for "Failed to allocate" messages
2. Ensure enough disk space for 4GB file
3. Try smaller VM (2GB RAM) to test

## Alternative: Simpler Approach

If building from source proves too complex, consider using VirtualBox's existing snapshot mechanism:

```bash
# Take snapshot while VM is running
VBoxManage snapshot "Ubuntu-Test-Haywire" take "haywire-snapshot" --pause

# Memory will be saved to .sav file
# Haywire can read this (snapshot-based, not live)
```

**Trade-off**: Not live updates, but no code modifications needed.

## Next Steps

Once VirtualBox is working with shared memory:

1. Install Ubuntu in the VM
2. Install pahole: `sudo apt-get install dwarves`
3. Extract kernel profile (see `docs/virtualbox_implementation_plan.md`)
4. Test full Haywire workflow (discovery, process selection, memory visualization)
5. Compare performance with QEMU

## Known Limitations

- **macOS ARM64**: VirtualBox on ARM64 macOS may have limited support/performance
- **Code signing**: Modified kexts require signing or SIP disabled
- **Maintenance**: Need to reapply patches for each VirtualBox update
- **Alternative**: QEMU with HVF acceleration may be simpler on macOS

## Summary

This guide provides the foundation for building VirtualBox 7.2.4 with the secret range patch on macOS ARM64. The actual source code examination and patch refinement will happen during implementation.

**Reality check**: Building VirtualBox from source on macOS ARM64 is non-trivial. If you encounter significant obstacles, QEMU with HVF remains the recommended path for macOS.
