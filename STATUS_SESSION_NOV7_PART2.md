# Windows Kernel Discovery Session - Nov 7, 2025 Part 2

## Summary

Attempted to test Windows kernel discovery implementation but encountered blocking issues. Work saved as patch and rolled back to working state.

## What Was Implemented

Complete Windows kernel discovery system with:
- **Windows EPROCESS scanner** - `src/windows/windows_kernel_discovery.cpp`
- **Architecture hint system** - x86_64 vs ARM64 memory layout selection
- **Factory pattern** - GuestOS enum with Linux/Windows support
- **Command-line flags** - `--guest-os windows` parameter

### Key Technical Details

**Architecture Defaults:**
- x86_64 (Windows): RAM at 0x0, default 8GB
- ARM64 (Linux): RAM at 0x40000000, default 4GB

**Windows Offsets (Build 26100):**
```cpp
active_process_links = 0x448
unique_process_id = 0x440
image_file_name = 0x5a8
directory_table_base = 0x28
```

## Issues Encountered

1. **Kernel Discovery Hangs** - Initialize() scanning 8GB memory blocked GUI startup
2. **QMP Connection Blocks** - Stale connections prevented new connections
3. **Memory View Black** - Even with `--no-qemu`, visualizer showed black screen
4. **Architecture Detection Working** - Memory layout correctly showed x86_64 (RAM at 0x0)

## Actions Taken

1. ✅ Commented out kernel discovery initialization to unblock GUI
2. ✅ Added `--no-qemu` flag to skip QMP entirely
3. ✅ Fixed connection window appearing with `--no-qemu`
4. ✅ Increased x86_64 default RAM from 2GB to 8GB
5. ❌ Memory visualizer still blank despite fixes

## Rollback Decision

**Problem**: Visualizer was working before Windows kernel discovery work started
**Solution**: Save work as patch and roll back to last known-good commit

### Patch Saved

`windows_kernel_discovery_nov7_2025.patch` - Contains all Windows discovery changes

### Files Modified (Rolled Back)

- `src/main.cpp` - Guest OS hint, Initialize() call commented out, noQemu flag
- `src/memory_mapper.cpp` - 8GB x86_64 default
- `src/kernel_discovery_backend.h/cpp` - GuestOS parameter
- `src/kernel_discovery_factory.cpp` - Windows case added
- `src/memory_backend.cpp` - arch_hint parameter
- `src/qemu_connection.cpp` - AutoConnect() with arch_hint
- `include/*` - Various header changes

### Reset Command

```bash
git reset --hard HEAD
```

Now at commit: `a0df47c Add Windows 11 VM setup with QEMU for Haywire introspection`

## Next Steps

1. **Test visualizer works** after rollback
2. **Debug black screen issue** separately from Windows work
3. **Reapply patch** once visualizer confirmed working
4. **Make kernel discovery optional/on-demand** to avoid blocking startup

## Lessons Learned

- Don't scan 8GB of memory synchronously during startup
- Need progress indicators for long operations
- QMP connection should have timeout/retry logic
- Test incrementally - visualizer broke but wasn't caught early
- `--no-qemu` mode valuable for testing without VM

## Windows 11 VM Status

- Running with 8GB RAM at `/tmp/haywire-vm-mem`
- QMP port: 4445
- Monitor port: 4444
- VNC port: 5901

## Build Status

Currently rebuilding after rollback...
