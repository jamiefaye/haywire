# VirtualBox + Haywire: Quick Start Guide

**Goal:** Make VirtualBox expose guest memory for Haywire introspection, WITHOUT recompiling VirtualBox.

## TL;DR - Run This First! ⚡

```bash
# 1. Install Python API
pip install vboxapi

# 2. Enable debugging on your VM
VBoxManage modifyvm ubuntu-test --debugger on

# 3. Start VM
VBoxManage startvm ubuntu-test --type headless

# 4. Test if memory API works
python3 test_vbox_memory_api.py ubuntu-test
```

**This takes 5 minutes and tells you if the no-compilation approach will work!**

---

## The Big Question

**Q:** Can we expose VirtualBox guest memory without recompiling VirtualBox?

**A:** Maybe! VirtualBox has `IMachineDebugger::readPhysicalMemory()` in its COM API, but it might not be implemented. The test script above will tell you.

---

## Two Paths Forward

### Path A: COM API Bridge (If Test Succeeds) ✅

**What you get:**
- No VirtualBox compilation needed
- Working in hours, not days
- ~10-100ms latency (polling-based)

**How it works:**
```
VirtualBox VM
    ↓ (COM API reads memory)
Memory Bridge Daemon (Python)
    ↓ (writes to file)
/tmp/haywire-vbox-mem (mmap'd)
    ↓ (Haywire reads)
Haywire Visualizer
```

**Implementation:** ~300 lines Python

---

### Path B: "Secret Range" Patch (If Test Fails) ❌

**What you get:**
- Zero latency (direct mmap access)
- Perfect performance
- Identical to QEMU memory-backend-file

**How it works:**
- Modify VirtualBox to allocate guest RAM in shared memory file
- Haywire mmaps this file directly
- No polling, no bridge daemon

**Implementation:** ~150 lines C, but requires compiling VirtualBox (2-3 days)

See `docs/virtualbox_secret_range_patch.md` for details.

---

## Key Insight from Your Research

From `docs/vbox_investigation_results.md`:

> **VirtualBox dumps include kernel structures that QEMU isolates!**
>
> 66,067 PGD candidates found in high memory (4GB+)
>
> VirtualBox's dumpvmcore includes the 4GB+ memory region that QEMU's
> memory-backend-file doesn't expose.

**Translation:** VirtualBox doesn't artificially restrict memory like QEMU does. If we can expose it continuously (not just dumps), we get **everything** including kernel structures.

---

## The Answer to Your Original Question

> "Is it possible to get VB to include the kernel structures 'out of bounds',
> so they actually become in bounds too?"

**YES!** They're already "in bounds" in VirtualBox's view. The challenge is just exposing them continuously:

- **Dumps:** Already include everything (your investigation proved this)
- **Live access:** Need either COM API (Path A) or patch (Path B)

The "out of bounds" restriction is QEMU-specific, not VirtualBox!

---

## Files Created for You

### Testing
- `test_vbox_memory_api.py` - **Run this first!** Tests if COM API works
- `docs/vbox_getting_started.md` - Detailed testing instructions

### Design Documents
- `docs/vbox_memory_bridge_design.md` - COM API bridge architecture (Path A)
- `docs/virtualbox_secret_range_patch.md` - Compilation patch design (Path B)

### Existing Research (Your Previous Work)
- `docs/vbox_investigation_results.md` - Dump analysis proving kernel structures are accessible
- `docs/vbox_source_reference.md` - VirtualBox source code locations
- `parse_vbox_dump.py` - ELF dump parser
- `analyze_vbox_memory.py` - PGD pattern scanner

---

## Comparison Table

| Approach | Time to Working | Performance | Maintenance | Compilation |
|----------|----------------|-------------|-------------|-------------|
| **COM API Bridge** | 1 day | Good (10-100ms) | Easy (Python) | None ✅ |
| **Secret Range Patch** | 3-4 days | Perfect (0ms) | Rebase on updates | Required ❌ |
| **Dumps (Current)** | Already works | Static only | None | None ✅ |
| **QEMU (Current)** | Already works | Perfect (0ms) | None | None ✅ |

---

## Recommendation

1. **Run the test script NOW** (5 minutes)
   ```bash
   python3 test_vbox_memory_api.py ubuntu-test
   ```

2. **If succeeds:** Build COM API bridge (Path A)
   - Quick win
   - Good enough for most use cases
   - No compilation headaches

3. **If fails:** Consider your options:
   - **Easy:** Keep using QEMU (already perfect!)
   - **Medium:** Use VirtualBox dumps (static analysis, already working)
   - **Hard:** Implement secret range patch (best VirtualBox performance, but 3-4 days work)

---

## Why Consider VirtualBox at All?

From your investigation, VirtualBox has some advantages:

✅ **Native Windows support** (no WSL2 needed)
✅ **Includes high memory** (4GB+ with kernel structures)
✅ **COM API** (programmatic control)
✅ **Good dump support** (ELF cores with all memory)

But QEMU with memory-backend-file already works great for Haywire!

---

## What to Do Right Now

```bash
# This is your "go/no-go" test
python3 test_vbox_memory_api.py ubuntu-test
```

Results tell you immediately whether the "no compilation" path is viable.

**Then come back and we'll implement whichever path makes sense!** 🚀

---

## Questions?

- Test script location: `test_vbox_memory_api.py`
- Detailed docs: `docs/vbox_getting_started.md`
- Design docs: `docs/vbox_memory_bridge_design.md`

Let's see what the test says!
