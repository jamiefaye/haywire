# VMware Setup for Haywire

## Finding the .vmem File

### VMware Fusion (macOS)
```bash
# List running VMs
ps aux | grep vmware-vmx

# Find VM directory
ls ~/Virtual\ Machines.localized/

# Look for .vmem file inside .vmwarevm bundle
ls ~/Virtual\ Machines.localized/Ubuntu\ ARM64.vmwarevm/*.vmem
```

Example path:
```
/Users/jamie/Virtual Machines.localized/Ubuntu ARM64.vmwarevm/Ubuntu ARM64.vmem
```

### VMware Workstation (Windows)
```
C:\Users\YourName\Documents\Virtual Machines\Ubuntu ARM64\Ubuntu ARM64.vmem
```

### VMware Workstation (Linux)
```
~/vmware/Ubuntu ARM64/Ubuntu ARM64.vmem
```

## Testing Haywire with VMware

### Quick Test

```bash
# 1. Boot your VMware VM

# 2. Find the .vmem file
VMEM_FILE="/Users/jamie/Virtual Machines.localized/Ubuntu ARM64.vmwarevm/Ubuntu ARM64.vmem"

# 3. Create symlink (Haywire expects /tmp/haywire-vm-mem)
ln -sf "$VMEM_FILE" /tmp/haywire-vm-mem

# 4. Run Haywire
cd /Users/jamie/haywire/build
./haywire
```

### Important Notes

**Memory File Behavior:**
- `.vmem` file only exists while VM is running
- VMware may not immediately flush changes to disk (cached in host RAM)
- For best results, take a snapshot to force memory to disk

**Taking a Snapshot (Forces Memory to Disk):**
1. In VMware: VM → Snapshot → Take Snapshot
2. Check "Snapshot the virtual machine's memory" ✓
3. This creates `.vmsn` (snapshot) and `.vmem` (memory dump)
4. The `.vmem` file will be more stable for analysis

## Getting Kernel Profile (No SSH Needed!)

### Method 1: VM Console (Works for all VMware versions)

1. **Access VM console** (just click in the VMware window)
2. **Login** at console
3. **Install pahole:**
   ```bash
   sudo apt-get install dwarves
   ```
4. **Extract offsets:**
   ```bash
   pahole -C task_struct /sys/kernel/btf/vmlinux > /tmp/profile.txt
   pahole -C mm_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
   pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /tmp/profile.txt
   cat /tmp/profile.txt
   ```
5. **Copy output** (VMware has clipboard integration - just select and copy)

### Method 2: Shared Folders (Easiest!)

1. **Enable shared folders in VMware:**
   - VM Settings → Sharing → Enable Shared Folders
   - Add folder: `/Users/jamie/haywire/shared` → `/mnt/hgfs/shared`

2. **Inside VM console:**
   ```bash
   sudo apt-get install open-vm-tools open-vm-tools-desktop
   sudo apt-get install dwarves

   # Write directly to shared folder
   pahole -C task_struct /sys/kernel/btf/vmlinux > /mnt/hgfs/shared/profile.txt
   pahole -C mm_struct /sys/kernel/btf/vmlinux >> /mnt/hgfs/shared/profile.txt
   pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /mnt/hgfs/shared/profile.txt
   ```

3. **On macOS host:**
   ```bash
   # File appears instantly!
   cat /Users/jamie/haywire/shared/profile.txt

   # Generate profile
   python3 scripts/create_kernel_profile.py < shared/profile.txt > profiles/vmware-ubuntu-arm64.json
   ```

### Method 3: Copy-Paste from Console

VMware console has clipboard integration:
1. Run pahole commands
2. Select output with mouse
3. Copy (Cmd+C)
4. Paste into Terminal on macOS host
5. Pipe to profile generator

## Running Haywire

### Option A: Symlink Method (Simple)
```bash
# Create symlink
ln -sf "/path/to/VM.vmwarevm/VM.vmem" /tmp/haywire-vm-mem

# Run haywire
./haywire
```

### Option B: Modify Code (Flexible)
Change the default memory file path in `main.cpp`:
```cpp
std::string memoryFile = "/Users/jamie/Virtual Machines.localized/Ubuntu.vmwarevm/Ubuntu.vmem";
```

### Option C: Command-Line Argument (Future Enhancement)
Could add:
```bash
./haywire --memory-file "/path/to/VM.vmem"
```

## Differences from QEMU

| Feature | QEMU | VMware |
|---------|------|--------|
| Memory file | `/tmp/haywire-vm-mem` | `.vmem` in VM directory |
| QMP access | ✓ Yes | ✗ No |
| Swapper PGD discovery | QMP or heuristic | Heuristic only |
| Live memory updates | Instant (MAP_SHARED) | Cached (may need snapshot) |
| Shared folders | Harder to set up | Built-in, easy |
| Console clipboard | No | Yes |

## Troubleshooting

### .vmem file not found
- Make sure VM is running
- Check VM settings for correct memory size
- Look inside .vmwarevm bundle on macOS

### Memory appears static/not updating
- VMware caches memory in host RAM
- Take a snapshot to force flush to disk
- Or: Suspend and resume VM

### Kernel offsets wrong
- Extract fresh profile using pahole in VM
- VMware VMs can run different kernels than QEMU

### Can't find swapper_pgd
- QMP not available on VMware
- Heuristic discovery should work automatically
- Check that profile offsets are correct for your kernel

## Next Steps

1. Boot VMware VM
2. Find .vmem file location
3. Extract kernel profile (console or shared folder)
4. Symlink .vmem to /tmp/haywire-vm-mem
5. Run Haywire!

The process is actually **easier than QEMU** because:
- VMware has better shared folder support
- Console has clipboard integration
- No QMP setup needed
