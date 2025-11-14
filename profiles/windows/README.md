# Windows Kernel Profiles

This directory contains kernel structure offset profiles for Windows guests.

## Quick Start

### Method 1: Use Pre-Built Profile (Easiest)

If you're running Windows 11 Build 26100 (24H2):
```bash
# Profile is already included!
./haywire --guest-os windows --profile profiles/windows/windows-11-26100-x86_64.json
```

### Method 2: Generate Profile from Running Windows VM

```bash
# From Linux host, pass Windows kernel to extraction script
python3 scripts/extract_windows_profile.py /path/to/ntoskrnl.exe

# Or from inside Windows VM:
python extract_windows_profile.py C:\Windows\System32\ntoskrnl.exe
```

The script will:
1. Extract PE debug info to find PDB GUID
2. Download PDB from Microsoft symbol server
3. Parse structure offsets (currently uses Vergilius Project fallback)
4. Generate JSON profile

## Profile Contents

Each profile contains:

### 1. **EPROCESS Offsets** (Process Discovery)
```json
"EPROCESS": {
  "UniqueProcessId": {"offset": 1088},      // PID
  "ActiveProcessLinks": {"offset": 1096},   // List traversal
  "ImageFileName": {"offset": 1448},        // Process name
  "VadRoot": {"offset": 1560}               // Memory maps
}
```

### 2. **KPROCESS Offsets** (Page Tables)
```json
"KPROCESS": {
  "DirectoryTableBase": {"offset": 40}      // CR3 / PGD
}
```

### 3. **MMVAD Offsets** (Memory Mapping - equivalent to vm_area_struct)
```json
"MMVAD_SHORT": {
  "StartingVpn": {"offset": 24},            // Start address >> 12
  "EndingVpn": {"offset": 28},              // End address >> 12
  "VadFlags": {"offset": 48}                // Permissions
}
```

### 4. **Page Table Format** (PTE/PDE bits)
Standard x86_64 4-level paging (same as Linux)

## Verifying Offsets

### Using WinDbg (Recommended)

```
# Inside Windows VM with Debugging Tools installed
kd> dt nt!_EPROCESS
kd> dt nt!_MMVAD
kd> dt nt!_KPROCESS
```

### Using Vergilius Project

Visit: https://www.vergiliusproject.com/

Select your Windows version and search for structures.

## Profile Naming Convention

```
windows-<version>-<build>-<arch>.json

Examples:
  windows-11-26100-x86_64.json   (Windows 11 24H2)
  windows-10-19045-x86_64.json   (Windows 10 22H2)
  windows-11-22631-x86_64.json   (Windows 11 23H2)
```

## Supported Windows Versions

- ✅ Windows 11 Build 26100 (24H2) - Verified
- ⚠️  Windows 11 Build 22631 (23H2) - Needs testing
- ⚠️  Windows 10 Build 19045 (22H2) - Needs testing

## How Haywire Uses Windows Profiles

### Process Discovery
1. Find `PsActiveProcessHead` (via pattern scanning or guest agent)
2. Walk `ActiveProcessLinks` list
3. For each EPROCESS:
   - Read `UniqueProcessId` → PID
   - Read `ImageFileName` → process name
   - Read `Pcb.DirectoryTableBase` → CR3 for VA→PA translation

### Memory Mapping (VADs)
1. Get `VadRoot` from EPROCESS
2. Walk AVL tree (Left/Right pointers in `RTL_BALANCED_NODE`)
3. For each MMVAD:
   - Read `StartingVpn` and `EndingVpn` → address range
   - Read `VadFlags` → permissions (R/W/X)
   - Read `Subsection` → file mapping info (DLLs, EXEs)

### Page Table Walking
Same as Linux - standard x86_64 4-level paging:
```
VA → CR3 → PML4[bits 47:39] → PDPT[bits 38:30] → PD[bits 29:21] → PT[bits 20:12] → PA
```

## Differences from Linux Profiles

| Feature | Linux | Windows |
|---------|-------|---------|
| Process list | Circular doubly-linked | Circular doubly-linked |
| Process struct | task_struct | EPROCESS |
| Memory maps | rb_tree (vm_area_struct) | AVL tree (MMVAD) |
| Page tables | 4-level paging | 4-level paging (same) |
| Profile source | BTF (`/sys/kernel/btf/vmlinux`) | PDB (Microsoft symbol server) |
| Update frequency | Every kernel update | Every Windows update |

## Troubleshooting

### "Profile not found" error
- Check profile filename matches Windows version
- Use `--profile path/to/profile.json` to specify manually

### Wrong offsets / crashes
- Generate fresh profile for your exact Windows build
- Run `winver` in Windows VM to get build number
- Compare with profile's `build` field

### PDB download fails
- Check internet connectivity
- Microsoft symbol server may be temporarily down
- Use Vergilius Project to manually extract offsets

## Contributing Profiles

If you've created a profile for a Windows version not in this directory:

1. Verify offsets with WinDbg: `dt nt!_EPROCESS`
2. Test process discovery works
3. Set `"verified": true` in JSON
4. Submit pull request with profile and testing notes

## Future Enhancements

- [ ] Automatic PDB parsing (currently uses Vergilius fallback)
- [ ] PsActiveProcessHead symbol discovery
- [ ] Support for WinDbg .kdbg files
- [ ] Profile database with common Windows versions
- [ ] Guest agent integration for automatic version detection
