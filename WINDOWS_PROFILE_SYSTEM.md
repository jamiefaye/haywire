# Windows Profile System - Implementation Complete

## What We Built

Extended Haywire's kernel profile system to support Windows guests, matching the Linux implementation.

## Files Created/Modified

### 1. Profile Infrastructure
- **`profiles/windows/windows-11-26100-x86_64.json`** - Complete Windows 11 Build 26100 profile
- **`profiles/windows/README.md`** - Documentation for Windows profiles
- **`scripts/extract_windows_profile.py`** - PDB extraction tool (with Vergilius fallback)

### 2. Profile Loader Extension
- **`src/kernel_profile_loader.h`** - Extended `KernelProfile` struct with Windows fields:
  - `eprocess_*` - EPROCESS structure offsets
  - `kprocess_*` - KPROCESS (embedded in EPROCESS)
  - `mmvad_*` - Memory mapping (equivalent to vm_area_struct)
  - Added OS detection (`os` field: "linux" or "windows")
  - Conditional parsing based on OS type

## Windows Profile Contents

### EPROCESS Offsets
```cpp
eprocess_unique_process_id = 1088;       // PID
eprocess_active_process_links = 1096;   // Doubly-linked list
eprocess_image_file_name = 1448;        // Process name (15 chars)
eprocess_vad_root = 1560;               // VAD tree root
eprocess_peb = 1608;                    // Process Environment Block
eprocess_size = 2816;                   // Structure size
```

### KPROCESS Offsets
```cpp
kprocess_directory_table_base = 40;     // CR3 / PGD
```

### MMVAD Offsets (Memory Mapping)
```cpp
mmvad_short_starting_vpn = 24;          // Start address >> 12
mmvad_short_ending_vpn = 28;            // End address >> 12
mmvad_short_vad_flags = 48;             // Permissions
```

## Next Steps to Complete Integration

### Step 1: Update Windows Kernel Discovery

Modify `src/windows/windows_kernel_discovery.cpp` to:

1. **Load profile in constructor:**
```cpp
WindowsKernelDiscovery(const std::string& memFile, const std::string& profilePath) {
    KernelProfile profile;
    if (!profilePath.empty()) {
        KernelProfileLoader::LoadProfile(profilePath, profile);
    } else {
        // Try default Windows 11 profile
        KernelProfileLoader::LoadProfile(
            "profiles/windows/windows-11-26100-x86_64.json", profile);
    }

    // Store profile for use
    this->profile = profile;
}
```

2. **Replace WindowsOffsets struct** with profile fields:
```cpp
// OLD:
const char* name = reinterpret_cast<const char*>(data + offsets.image_file_name);

// NEW:
const char* name = reinterpret_cast<const char*>(data + profile.eprocess_image_file_name);
```

3. **Update all offset references** throughout:
- `offsets.unique_process_id` → `profile.eprocess_unique_process_id`
- `offsets.active_process_links` → `profile.eprocess_active_process_links`
- `offsets.directory_table_base` → `profile.kprocess_directory_table_base`
- etc.

### Step 2: Wire Up Profile Path

Update `src/kernel_discovery_factory.cpp` to pass profile path:
```cpp
case GuestOS::Windows:
    return std::make_unique<WindowsKernelDiscovery>(
        memFile,
        "profiles/windows/windows-11-26100-x86_64.json"
    );
```

### Step 3: Add Profile Auto-Detection (Optional)

Could add version detection:
```cpp
std::string DetectWindowsVersion() {
    // Read kernel banner or version info from memory
    // Match against available profiles
    return "profiles/windows/windows-11-26100-x86_64.json";
}
```

## Benefits

### ✅ Version Independence
- No more hardcoded offsets
- Easy to support multiple Windows versions
- Just add new JSON profiles

### ✅ Parity with Linux
- Same profile system for both OSes
- Consistent architecture
- Single source of truth for structure layouts

### ✅ Maintainability
- Offsets documented in human-readable JSON
- Can be updated without recompilation
- Easy to verify against WinDbg/PDB files

### ✅ Extensibility
- Can add more structures (ETHREAD, TOKEN, etc.)
- Support for symbols/globals (PsActiveProcessHead)
- Room for PTE layouts, pool tags, etc.

## Testing

Once Windows kernel discovery is updated to use profiles:

```bash
# Build with profile support
cmake --build build-linux

# Run with Windows guest
./haywire --guest-os windows --profile profiles/windows/windows-11-26100-x86_64.json

# Or use auto-detection (once implemented)
./haywire --guest-os windows
```

## Future Enhancements

1. **Full PDB Parsing** - Replace Vergilius fallback with real PDB parser
2. **Profile Database** - Community-contributed profiles for common Windows versions
3. **Symbol Support** - Load global symbols (PsActiveProcessHead, etc.) from PDB
4. **Guest Agent Integration** - Auto-detect Windows version from running VM
5. **VAD Tree Walking** - Use MMVAD offsets to enumerate process memory sections
6. **Page Table Walking** - Use CR3 from DirectoryTableBase for VA→PA translation

## Comparison: Linux vs Windows Profiles

| Feature | Linux | Windows |
|---------|-------|---------|
| **Profile Source** | BTF (`/sys/kernel/btf/vmlinux`) | PDB (Microsoft symbol server) |
| **Extraction Tool** | `pahole` | `extract_windows_profile.py` |
| **Process Structure** | `task_struct` | `EPROCESS` |
| **Memory Maps** | `vm_area_struct` (rb_tree) | `MMVAD` (AVL tree) |
| **Page Directory** | `mm_struct.pgd` | `KPROCESS.DirectoryTableBase` |
| **Process List** | `task_struct.tasks` | `EPROCESS.ActiveProcessLinks` |
| **Update Frequency** | Every kernel update | Every Windows update |

Both use the same JSON profile format and loading mechanism!

## Status

- ✅ Profile system extended for Windows
- ✅ Windows 11 Build 26100 profile created
- ✅ Profile loader supports both Linux and Windows
- ✅ Documentation complete
- ⏳ Windows kernel discovery integration (next step)
- ⏳ Testing with live Windows 11 VM

The foundation is complete. Next session can focus on updating the Windows kernel discovery code to use these profiles.
