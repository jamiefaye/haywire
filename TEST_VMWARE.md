# VMware Testing Quick Start

## Prerequisites
- [ ] VMware Fusion installed (or Workstation on Windows/Linux)
- [ ] Ubuntu ARM64 VM created and booted
- [ ] Haywire compiled (`make` in build directory)

## Step-by-Step Testing

### 1. Find the VMware Memory File (5 minutes)

```bash
# Find your running VM's memory file
# On macOS (Fusion):
ls ~/Virtual\ Machines.localized/*/*.vmem

# Example output:
# /Users/jamie/Virtual Machines.localized/Ubuntu ARM64.vmwarevm/Ubuntu ARM64.vmem
```

**Save this path!** You'll need it in step 3.

### 2. Extract Kernel Profile (10 minutes)

**Option A: Shared Folder (Recommended)**
```bash
# 1. In VMware: VM Settings → Sharing → Enable Shared Folders
#    Add: /Users/jamie/haywire/shared → /mnt/hgfs/shared

# 2. In VM console (just click in VMware window, login):
sudo apt-get install -y open-vm-tools dwarves
pahole -C task_struct /sys/kernel/btf/vmlinux > /mnt/hgfs/shared/p.txt
pahole -C mm_struct /sys/kernel/btf/vmlinux >> /mnt/hgfs/shared/p.txt
pahole -C vm_area_struct /sys/kernel/btf/vmlinux >> /mnt/hgfs/shared/p.txt

# 3. On macOS host:
cd /Users/jamie/haywire
python3 scripts/create_kernel_profile.py < shared/p.txt > profiles/vmware-ubuntu.json
```

**Option B: Copy-Paste (If shared folders don't work)**
```bash
# 1. In VM console:
sudo apt-get install -y dwarves
pahole -C task_struct /sys/kernel/btf/vmlinux
pahole -C mm_struct /sys/kernel/btf/vmlinux
pahole -C vm_area_struct /sys/kernel/btf/vmlinux

# 2. Select output with mouse, copy (Cmd+C works in VMware console!)

# 3. On macOS host:
cd /Users/jamie/haywire
python3 scripts/create_kernel_profile.py
# (Paste the output, then Ctrl-D)
# Save output to profiles/vmware-ubuntu.json
```

### 3. Run Haywire with VMware Memory

```bash
cd /Users/jamie/haywire

# Create symlink to VMware memory file
ln -sf "/Users/jamie/Virtual Machines.localized/Ubuntu ARM64.vmwarevm/Ubuntu ARM64.vmem" /tmp/haywire-vm-mem

# Verify symlink
ls -lh /tmp/haywire-vm-mem
# Should show: /tmp/haywire-vm-mem -> /Users/.../Ubuntu ARM64.vmem

# Run Haywire with VMware profile
cd build
./haywire
# Or with explicit profile:
# ./haywire --profile ../profiles/vmware-ubuntu.json  (if we add this option)
```

### 4. Verify It's Working

**Expected output:**
```
Memory mapped: 4096 MB
No kernel profile specified, using built-in defaults
  (or: Auto-detected kernel profile: profiles/vmware-ubuntu.json)

=== Process Discovery ===
Full memory scan for task_structs...
Found 478 processes

=== Extracting Process PGDs ===
No swapper PGD from QMP - attempting heuristic discovery...
Searching for swapper PGD with improved scoring...

Functional testing top candidates against discovered processes...
  Candidate 1 (0x136dec000): 19/20 (95%)  ← Should see high success rate
  ...

Selected swapper PGD via functional testing: 0x136dec000
```

**In the UI:**
- Process list should populate (click Select button or press S)
- Memory visualization should show data
- Heat map should work (press H)

## Troubleshooting

### "Failed to open memory file"
```bash
# Check if .vmem exists
ls -lh "/Users/jamie/Virtual Machines.localized/Ubuntu ARM64.vmwarevm/Ubuntu ARM64.vmem"

# Make sure VM is running (file only exists when VM is on)

# Check symlink
ls -lh /tmp/haywire-vm-mem
```

### "No processes found" or "0/20 (0%)" success rate
```bash
# Profile offsets might be wrong
# Re-extract profile from running VM:
# (repeat step 2)

# Or: Try with built-in defaults first
# (they're for Ubuntu 6.14.0-34 ARM64)
```

### Shared folders not working
```bash
# In VM:
sudo apt-get install -y open-vm-tools open-vm-tools-desktop
sudo mkdir -p /mnt/hgfs
sudo vmhgfs-fuse .host:/ /mnt/hgfs -o allow_other
ls /mnt/hgfs

# If still not working: Use copy-paste method instead
```

### Memory seems frozen/not updating
VMware caches memory in host RAM. Two solutions:
1. **Take a snapshot** (VM → Snapshot → Take Snapshot with memory)
2. **Suspend and resume** (forces flush to disk)

## Success Criteria

- ✓ Haywire starts without errors
- ✓ Process list shows 400+ processes
- ✓ Can select a process (e.g., systemd)
- ✓ Memory view shows data (not all black)
- ✓ Swapper PGD discovery has >80% success rate

## Performance Notes

**VMware vs QEMU:**
- **Slower**: VMware caches memory in host RAM (not always flushed to .vmem)
- **Easier setup**: Shared folders "just work", console clipboard works
- **No QMP**: Must rely on heuristic swapper_pgd discovery
- **Same format**: .vmem is identical to QEMU memory-backend-file

**For best performance:**
- Take a snapshot before running Haywire (forces memory to disk)
- Or: Run against snapshot files (.vmsn/.vmem pair)

## Next: Test Other VM Vendors

Once VMware works, try:
- **VirtualBox**: Uses `.sav` files or VBoxManage debugvm
- **Hyper-V**: Uses `.bin` memory dumps
- **Parallels**: Uses `.mem` files

All follow same process:
1. Find memory file location
2. Extract kernel profile
3. Symlink to /tmp/haywire-vm-mem
4. Run Haywire

See `docs/vmware_setup.md` for detailed information.
