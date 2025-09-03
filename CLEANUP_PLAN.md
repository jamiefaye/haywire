# Haywire Cleanup Plan - Remove External Hacking Code

## Files to Remove/Revert in QEMU

### 1. Main Haywire Tracking Code
- `/Users/jamie/haywire/qemu-mods/qemu-src/target/arm/haywire-tracker.c` - DELETE
- `/Users/jamie/haywire/qemu-mods/qemu-src/target/arm/haywire-tracker.h` - DELETE  
- `/Users/jamie/haywire/qemu-mods/qemu-src/qapi/haywire.json` - DELETE

### 2. HVF Hook Modifications
- `/Users/jamie/haywire/qemu-mods/qemu-src/target/arm/hvf/hvf.c` - REVERT
  - Remove the haywire_context_switch_hook calls
  - Remove the aggressive sampling code

### 3. Helper.c Modifications  
- `/Users/jamie/haywire/qemu-mods/qemu-src/target/arm/helper.c` - REVERT
  - Remove vmsa_ttbr_write hooks
  - Remove any #ifdef HAYWIRE sections

### 4. Build System Changes
- `/Users/jamie/haywire/qemu-mods/qemu-src/target/arm/meson.build` - REVERT
  - Remove haywire-tracker.c from sources
- `/Users/jamie/haywire/qemu-mods/qemu-src/qapi/meson.build` - REVERT
  - Remove haywire.json from QAPI schemas

### 5. QMP Command Additions
- `/Users/jamie/haywire/qemu-mods/qemu-src/qapi/misc.json` - CHECK
  - Remove any Haywire-related commands if added there

## Files to Remove from Haywire Project

### Test/Research Scripts (DELETE ALL)
```bash
/tmp/find_task_list.py
/tmp/find_init_task.py  
/tmp/find_processes.py
/tmp/walk_init_task.py
/tmp/walk_real_processes.py
/tmp/find_kworker_comm.py
/tmp/find_group_leader.py
/tmp/check_thread_theory.py
/tmp/find_comm_offset.py
/tmp/check_haywire_*.py
/tmp/list_real_processes.py
/tmp/show_haywire_success.py
```

### C++ Files (Consider Keeping for Reference)
- `walk_process_list.cpp` - Maybe keep as reference
- `find_processes_qmp.cpp` - Maybe keep as reference

## What to KEEP

### 1. Documentation
- `VM_INJECTION_TECHNIQUES.md` - KEEP (new approach)
- `docs/` folder - KEEP (useful documentation)
- `CLAUDE.md` - UPDATE to reflect new approach

### 2. Launch Scripts  
- `scripts/launch_qemu_membackend.sh` - KEEP (still useful)
- Remove Haywire-specific modifications

### 3. Cloud-Init Files
- `/tmp/seed.iso` - KEEP (for testing)
- `/tmp/user-data` - KEEP
- `/tmp/meta-data` - KEEP

## Quick Cleanup Commands

```bash
# Remove test scripts
rm -f /tmp/*haywire*.py /tmp/find_*.py /tmp/walk_*.py /tmp/check_*.py

# Revert QEMU to clean state
cd /Users/jamie/haywire/qemu-mods/qemu-src
git status  # See what's modified
git diff    # Review changes

# Option 1: Revert everything
git checkout -- .

# Option 2: Selective revert
git checkout -- target/arm/hvf/hvf.c
git checkout -- target/arm/helper.c
git checkout -- target/arm/meson.build
git checkout -- qapi/meson.build
rm target/arm/haywire-tracker.*
rm qapi/haywire.json

# Rebuild clean QEMU
cd build
ninja clean
ninja
```

## Testing with Stock QEMU

Once cleaned up, you can test with stock QEMU:
```bash
# Just use regular qemu-system-aarch64 from Homebrew
brew install qemu

# Launch with same options minus Haywire stuff
qemu-system-aarch64 \
    -M virt \
    -accel hvf \
    -cpu host \
    -m 4G \
    ... (rest of normal options)
```

## New Approach Summary

Instead of:
- Hooking context switches
- Reading kernel memory
- Guessing struct offsets
- Fighting security features

We'll use:
- Cloud-init injection
- Guest agent writing to shared memory
- Official /proc APIs
- Cross-platform compatible

## Ready to Clean?

1. First backup anything you want to keep
2. Run the cleanup commands
3. Rebuild QEMU (or switch to stock)
4. Test cloud-init injection approach