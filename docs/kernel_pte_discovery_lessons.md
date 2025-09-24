# Kernel PTE Discovery - Lessons Learned

## Date: September 24, 2025

### The Problem
We had 126 processes discovered but were finding 0 PTEs, despite having working page table walking code just yesterday. The kernel page tooltips couldn't work without mapping memory pages to their owning processes.

## Key Discoveries

### 1. Page Table Walking Was Broken by Offset Removal
**Issue**: We had removed GUEST_RAM_START offset adjustments thinking they were unnecessary.

**Reality**: ARM64 physical addresses have two distinct ranges in QEMU:
- **0x0 - 0x40000000 (1GB)**: Low memory region - directly accessible in memory file at offset 0x0
- **0x40000000+ (1GB+)**: Main RAM - accessible at `(PA - GUEST_RAM_START)` offset

**Fix**:
```typescript
// For high physical addresses (>= 1GB), subtract GUEST_RAM_START
const pudOffset = pudPhysAddr >= KernelConstants.GUEST_RAM_START
    ? pudPhysAddr - KernelConstants.GUEST_RAM_START
    : pudPhysAddr;
```

### 2. PGD Walking Was Only Checking Half the Entries
**Issue**: Only checking 256 entries instead of full 512 in PGD table

**Impact**: Missing kernel space entries (256-511) where kernel mappings live

**Fix**: Extended loops from 256 to 512 entries

### 3. Kernel PGD Changes on Reboot (KASLR)
**Discovery**: The kernel PGD address (swapper_pg_dir) changes between boots due to KASLR

**Impact**: Hardcoded addresses like 0x136dbf000 become stale after VM reboot

**Solution**: Dynamic discovery via QMP to get ground truth from CPU registers

### 4. QMP Integration in Electron Requires IPC
**Challenge**: Electron renderer process can't use Node.js `net` module directly

**Initial Attempt**: Dynamic import of qmp-client module - caused "Failed to fetch" errors

**Solution Architecture**:
1. Main process: Handles QMP connection using native net.Socket
2. Preload script: Exposes QMP operations via contextBridge
3. Renderer: Calls QMP via electronAPI.queryKernelInfo()

### 5. Physical Address Interpretation
**Key Insight**: Physical addresses in page table entries need different handling:
- Addresses < 0x40000000: Direct offset in memory file
- Addresses >= 0x40000000: Subtract GUEST_RAM_START for file offset

This affects PUD, PMD, and PTE table reading when walking page tables.

### 6. QMP Provides Ground Truth
**QMP query-kernel-info**: Returns TTBR1_EL1 register containing kernel PGD
- More reliable than heuristic discovery
- Survives KASLR/reboots
- Validates our page table walking

## Results After Fixes

### Before
- 126 processes found
- **0 PTEs discovered**
- No page-to-process mapping
- Kernel tooltips impossible

### After
- 127 processes found
- **33 processes with PTEs**
- 79,864 pages tracked
- 154,836 total mappings
- 23,657 shared pages identified
- Kernel tooltips now possible!

## Technical Details

### QMP Protocol Flow
1. Connect to QEMU on port 4445 (not 4444 which is human monitor)
2. Send `qmp_capabilities` to initialize
3. Send `query-kernel-info` with cpu-index 0
4. Extract TTBR1_EL1 and mask to get PGD physical address

### Electron IPC Pattern
```javascript
// Main process
ipcMain.handle('qmp:queryKernelInfo', async () => {
    const socket = new net.Socket();
    // ... QMP protocol implementation
    return { success: true, kernelPgd };
});

// Preload
contextBridge.exposeInMainWorld('electronAPI', {
    queryKernelInfo: () => ipcRenderer.invoke('qmp:queryKernelInfo')
});

// Renderer
const result = await window.electronAPI.queryKernelInfo();
```

### Memory Layout Understanding
```
Physical Address Space:
0x00000000 - 0x3FFFFFFF: Low memory (1GB) - direct mapped at offset 0
0x40000000 - 0x1FFFFFFFF: Main RAM (up to 6GB) - mapped at (PA - 0x40000000)

File Offset Calculation:
if (PA < 0x40000000) {
    offset = PA;  // Direct mapping
} else {
    offset = PA - GUEST_RAM_START;  // Subtract base
}
```

## Debugging Commands That Helped

```bash
# Test QMP connection
node test-qmp-electron.mjs

# Find PGDs with correct signature (1 user, 2-3 kernel entries)
node find-real-swapper.mjs

# Walk page tables from QMP-provided PGD
node test-qmp-pgd-walk.mjs

# Monitor discovery progress
tail -f /tmp/haywire-kernel-discovery.log
```

## Key Takeaways

1. **Always validate assumptions about memory layout** - The PA offset handling was critical
2. **Use authoritative sources when available** - QMP provides ground truth vs heuristics
3. **Electron architecture matters** - Can't just import Node modules in renderer
4. **Full table walking is essential** - Don't assume partial walks are sufficient
5. **KASLR breaks hardcoded addresses** - Need dynamic discovery mechanisms
6. **Test scripts are invaluable** - Small focused scripts helped isolate each issue

## Next Steps

With PTE discovery working, we can now:
1. Implement the kernel page tooltip system
2. Show which processes are using each memory page
3. Visualize shared memory between processes
4. Track page usage patterns over time