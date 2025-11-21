# Windows Guest Process Discovery - November 12, 2025 Part 4 (QMP Investigation)

## Session Summary

### Major Discovery: Build Version Mismatch
**Problem**: Profile claimed to be for Build 26100 (24H2), but guest is running Build 26200 (25H2)
- User confirmed: "version 25H2 OS build 26200.7171"
- This explained all the inconsistent offset errors

### Profile Corrections Made
All offsets verified against Vergilius Project for Windows 11 25H2:

**EPROCESS:**
- Size: 2816 → **2112** (0x840) ✓
- ProcessLock: 1080 → **456** (0x1c8) ✓
- Pcb.size: 1080 → **456** ✓
- UniqueProcessId: **464** ✓ (already fixed)
- ActiveProcessLinks: **472** ✓ (already fixed)
- ImageFileName: **824** ✓ (already fixed)
- VadRoot: **1368** ✓ (already fixed)
- Peb: **736** ✓ (already fixed)

**KPROCESS:**
- Size: 1080 → **456** (0x1c8) ✓
- DirectoryTableBase: **40** ✓ (already correct)

**MMVAD:**
- Size: 128 → **136** (0x88) - NOT committed (interrupted)
- All field offsets correct ✓

**Profile renamed** back to `windows-11-26100-x86_64.json` (code expects this filename)
- Metadata updated to reflect Build 26200 (25H2)

## Current Working Status

### ✅ What Works (WITHOUT --no-qemu flag):
- Process discovery: 100+ processes found
- PID accuracy: All 4 mspaint PIDs match Task Manager (5536, 5540, 5552, 8944)
- Process names: Correct (csrss.exe, dwm.exe, lsass.exe, etc.)
- Profile loading: Correct offsets now used
- QMP connection: Connects successfully to localhost:4445

### ❌ Current Blocker: Memory Beyond 8GB
**The Core Problem:**
- Windows allocates page tables BEYOND the 8GB memory-backend-file
- Example: PT at 0x26aeece40 (9.67 GB) - outside 8GB memory file
- VadRoot at 0xffffb10f917c8620 requires translation through PT at 9.67 GB

**QMP Can Access It:**
- Python test confirmed: `xp /8xb 0x26aeece40` returns valid data
- Bytes: `0x63 0xf9 0x99 0x1a 0x00 0x00 0x00 0x8a` (= 0x8a0000001a99f963)

**But C++ Code Doesn't Use It:**
- Added debug output: `[QMP] Reading beyond 8GB: addr=0x...`
- Result: **NO debug messages appear**
- Conclusion: QMP fallback is never triggered
- Code has the fallback logic (lines 483-497) but something prevents it from running

## Investigation Findings

### Why QMP Fallback Isn't Triggered
Possible reasons:
1. Code path doesn't attempt VA translation during discovery
2. ActiveProcessLinks walking not attempted (falls back to blind scan only)
3. VAD tree walking not implemented yet
4. TranslateVA() might not be called during discovery phase

### What's NOT Being Attempted
- ActiveProcessLinks list walking (message: "Cannot walk process list")
- VAD tree enumeration (no code path exists)
- PTE extraction (not implemented)
- VA→PA translation for kernel VAs

## Next Steps (For Future Sessions)

### Immediate Action Items
1. **Verify QMP fallback works**: Write a test that FORCES a read beyond 8GB
2. **Check why list walking isn't attempted**: Find where "Cannot walk" decision is made
3. **Implement VAD tree walker**: Create code to walk VAD tree WITH QMP for beyond-8GB access

### Path Forward Options

**Option A: Make QMP Work (Recommended)**
- QMP CAN access memory beyond 8GB (proven with Python)
- C++ has fallback logic already (just not triggered)
- Need to find why it's not being called
- Implement VA translation that uses QMP for page tables

**Option B: Accept Limitations**
- Stick with blind scanning (works, finds 100+ processes)
- PA mode only (no VA translation needed)
- Document that DLL enumeration requires QMP integration

**Option C: Alternative Memory Access**
- Check if QEMU has other memory exposure methods
- Investigate GDB protocol (already has some support)
- Look at memory-backend-file configuration options

## Files Modified This Session
1. `profiles/windows/windows-11-26100-x86_64.json`:
   - Updated metadata to Build 26200 (25H2)
   - Fixed EPROCESS size: 2816 → 2112
   - Fixed ProcessLock offset: 1080 → 456
   - Fixed Pcb size: 1080 → 456
   - Fixed KPROCESS size: 1080 → 456

2. `src/windows/windows_kernel_discovery.cpp`:
   - Added QMP debug output (lines 485-495)
   - Debug shows QMP connects but fallback never triggered

## Key Insights

1. **Always check guest OS version** - Build mismatches cause subtle offset errors
2. **Vergilius Project is reliable** - All verified offsets were correct for 25H2
3. **QMP works for x86_64** - Python test confirmed `xp/` command works
4. **Code has fallback logic** - Just need to trigger it properly
5. **Discovery doesn't attempt translation** - Explains why QMP fallback unused

## Memory File Location
- WSL path: `/tmp/haywire-vm-mem` (8GB)
- Windows path: `\\wsl$\Ubuntu\tmp\haywire-vm-mem`
- Size: 8589934592 bytes = 8 GB exactly
- Covers guest RAM: 0x0 - 0x1FFFFFFFF (8GB)
- Page tables beyond: 0x26aeece40+ (9.67 GB+)

## Questions for Next Session
1. Where does code decide NOT to walk ActiveProcessLinks?
2. Can we force TranslateVA() to be called during discovery?
3. Is there a way to expose more physical memory via memory-backend-file?
4. Should we implement VAD walking as a separate feature?

## Session End Status
✅ Process discovery fully working (100+ processes, correct PIDs)
✅ Profile offsets corrected for Build 26200
✅ QMP connection working
❌ QMP fallback not being triggered
❌ Cannot enumerate DLLs or PTEs (requires beyond-8GB access)

**Recommendation**: Focus next session on understanding why VA translation isn't attempted during discovery, then force it to trigger the QMP fallback.
