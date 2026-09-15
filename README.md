# Haywire

Haywire is a VM memory introspection tool. It reads a guest VM's RAM directly
from the host — through QEMU's `memory-backend-file` — and reconstructs kernel
and process structures **without any cooperation from the guest**: no agent, no
driver, no SSH into the VM. It then visualizes that memory: process maps, shared
libraries, page tables, and raw bytes rendered as images, hex, or disassembly.

> ⚠️ **Security:** Haywire deliberately breaks VM isolation. It can read
> anything in guest RAM — credentials, keys, keyrings. **Never run sensitive
> workloads in a VM you are introspecting with Haywire.** This is a research
> tool; use at your own risk.

## Two versions

| | **Web** (Vue / TypeScript) | **Native** (C++) |
|---|---|---|
| Runs | In a browser | macOS / Linux desktop |
| Best for | Exploring memory files and dumps, cross-platform | Real-time introspection of a live VM |
| Live change detection / heat map | ✗ (removed June 2026) | ✓ |
| Setup | `npm install` + a memory file to open | Build from source + running QEMU VM |

The web version is the easy, cross-platform explorer. The native version is the
real-time tier with a background patrol thread, live change detection, and heat
map visualization. (An Electron wrapper for the web version was deprecated in
June 2026.)

## Quick start — web

```bash
cd web
npm install
npm run dev        # dev server; open the printed URL
# or: npm run build && npm run preview
```

Then open a memory file (a QEMU `memory-backend-file`, or a saved dump) in the
UI. No VM is required to explore a static dump.

## Quick start — native

Requirements: CMake 3.16+, a C++17 compiler, OpenGL 3.2+, GLFW3, Capstone.

```bash
# macOS
brew install cmake glfw capstone
# Ubuntu/Debian
sudo apt-get install build-essential cmake libglfw3-dev libgl1-mesa-dev libcapstone-dev

cmake -B build && cmake --build build
```

Launch a VM whose RAM is backed by a shared `memory-backend-file` (see the
launch scripts in `scripts/`), then run:

```bash
./build/haywire                      # auto-detect guest OS
./build/haywire --guest-os windows   # force Windows discovery
./build/haywire --no-qemu --memory-file <dump>   # explore a static dump
```

The VM must expose its RAM via `-object memory-backend-file,...,share=on`
mapped to `/tmp/haywire-vm-mem`. The `scripts/launch_*.sh` files set this up for
Linux and Windows guests on macOS and Linux hosts.

## Platform support

- **Guests:** Linux (ARM64, x86-64) is the primary target. Windows 11 (x86-64)
  discovery works but is experimental.
- **Hosts:** macOS (Apple Silicon; x86 guests run under slow TCG emulation) and
  Linux (native KVM where the guest arch matches).
- macOS *guests* are not supported.

## Kernel profiles

Process/kernel discovery relies on per-kernel structure offsets, stored as JSON
in `profiles/`. Offsets are extracted from the running kernel's embedded BTF
(`/sys/kernel/btf/vmlinux`) with `pahole` — no kernel source or headers needed.
See `profiles/README.md` and `scripts/create_kernel_profile.py` to add a profile
for a new kernel.

## Documentation

Start with `docs/vm_setup_guide.md`. Current technical references:

- `docs/memory-map-visual.md` — physical memory layout and the RAM→file mapping
- `docs/rendering_pipeline.md` — the rendering pipeline and column mode
- `docs/address_notation.md` — the address-notation system
- `docs/build_qemu.md` — building the modified QEMU

Other files under `docs/` are research notes and may be historical — some are
banner-marked obsolete. Verify against the code before relying on them.
