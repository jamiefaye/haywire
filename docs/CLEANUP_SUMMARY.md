# Codebase Cleanup Summary

## Directories Organized:

### `obsolete/`
- Contains all beacon and companion-related files
- These components have been replaced by kernel discovery
- Includes documentation about the old architecture
- Files temporarily copied back to src/ to keep build working until dependencies are removed

### `test_attempts/`
- All test programs and their source files
- Experimental code and prototypes
- Test executables have been deleted to save space
- Source files preserved for reference
- Analysis documentation and notes

### `docs/`
- Moved important documentation here
- ARM64_ADDRESS_MANIPULATIONS.md
- KERNEL_DISCOVERY_BRAINDUMP.md

## Main Codebase Status:
- Kernel discovery fully integrated and working
- Gets swapper PGD from QMP
- Discovers all processes by scanning memory
- Extracts PGDs, memory sections, and PTEs
- Beacon files temporarily restored until dependencies removed
- See TODO_REMOVE_BEACON.md for cleanup plan

## Untracked Files Remaining:
- QEMU patches in `qemu-mods/`
- VM images in `vms/`
- SSL certificates in `ssl stuff/`
- Various test files in `test_attempts/`
- Obsolete beacon/companion code in `obsolete/`

## Space Saved:
- Removed 107GB of git temporary pack files
- Deleted all test executables
- Repository now at manageable size

## Next Steps:
1. Remove beacon dependencies (see TODO_REMOVE_BEACON.md)
2. Delete beacon source files once dependencies removed
3. Consider what to do with QEMU patches
4. Clean up or gitignore VM images