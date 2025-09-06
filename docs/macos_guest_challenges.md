# Running macOS as QEMU Guest with Haywire

## Architecture: Linux Host → QEMU → macOS Guest

### What Works
- **Beacon Protocol**: Memory-backend-file still functions normally
- **Physical Memory Access**: QEMU's memory mapping unchanged
- **Haywire Scanner**: Reads beacons from `/tmp/haywire-vm-mem` as usual

### Key Differences from Linux Guest

#### 1. No /proc Filesystem
macOS companion must use native APIs:
```c
sysctl()           // Process enumeration
proc_pidinfo()     // Process details
proc_name()        // Process names  
mach_vm_region()   // Memory maps
task_for_pid()     // Access other processes
```

#### 2. Permission Model
- **task_for_pid()** requires:
  - Root privileges OR
  - com.apple.security.get-task-allow entitlement OR
  - Code signature with debugging entitlements
- Self-inspection always works
- Other processes need elevated privileges

#### 3. Memory Layout Differences
- XNU kernel vs Linux kernel structures
- Mach-O binaries vs ELF
- Different virtual memory organization
- No /proc/PID/maps equivalent

#### 4. Beacon Discovery Challenges
In QEMU guest memory:
- Need to find beacon pages in guest physical memory
- macOS ASLR randomizes addresses
- mmap() addresses unpredictable
- Solution: Allocate with distinctive pattern for scanning

### "Hacky Things" Required

#### 1. Beacon Memory Allocation
```c
// Make beacons findable by repeating magic
void* allocate_findable_memory() {
    void* mem = mmap(NULL, size, ...);
    
    // Write repeated magic for easier discovery
    for (int i = 0; i < 16; i++) {
        ((uint32_t*)mem)[i] = BEACON_MAGIC;
    }
    return mem;
}
```

#### 2. Bypass Code Signing
Options:
- Run companion as root (simplest)
- Ad-hoc sign with entitlements:
  ```bash
  codesign -s - --entitlements entitlements.plist companion_macos
  ```
- Disable SIP in guest (not recommended)

#### 3. Handle Missing APIs
Some Linux /proc data has no macOS equivalent:
- Detailed memory permissions
- File backing for memory regions
- Some CPU timing statistics
- Process state details

#### 4. QEMU Configuration
Running macOS in QEMU requires:
- Custom OVMF/Clover bootloader
- Specific CPU model (Penryn/Haswell)
- VirtIO or e1000 network
- Careful machine type selection

### Implementation Strategy

#### Phase 1: Basic Process List
- Use sysctl() for process enumeration
- Run as root to avoid entitlements
- Pack into beacon format

#### Phase 2: Memory Regions (if needed)
- Requires task_for_pid() success
- Use mach_vm_region() iteration
- Limited info vs Linux /proc/maps

#### Phase 3: Optimization
- Cache unchanging data
- Batch syscalls where possible
- Minimize beacon updates

### Legal Considerations
Running macOS in QEMU may violate Apple's EULA unless:
- Running on Apple hardware
- Using for security research
- Personal use only

### Technical Limitations
1. **Performance**: macOS in QEMU is slow
2. **Graphics**: Limited acceleration
3. **Stability**: Kernel panics possible
4. **Updates**: OS updates often break

### Alternative Approaches
Instead of companion in guest:
1. **Direct QEMU introspection**: Read XNU structures from host
2. **QEMU patches**: Export process info via QMP
3. **Virtio channel**: Custom device for data export
4. **Network protocol**: Send data via TCP/UDP

### Recommendation
For macOS guests, consider:
- Keep companion minimal (process list only)
- Run with elevated privileges
- Use network channel as backup
- Focus on stability over features

The beacon protocol remains the same - only data collection changes.