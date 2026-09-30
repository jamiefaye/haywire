# Porting Haywire to a New Host

Lessons from moving Haywire from an M3 Mac to an Intel Mac (i9-9980HK, macOS 15.8
Sequoia, QEMU 11.1.1) on 2026-09-30. Most of the time went into QEMU/HVF quirks and
package-manager problems, not Haywire itself. Read the checklist first, then the
symptom tables when something breaks.

## Checklist

1. **Clone and build Haywire**
   ```bash
   git clone git@github.com:jamiefaye/haywire.git && cd haywire
   git submodule update --init lib/imgui      # NOT qemu-mods/qemu-src unless you need it
   cmake -B build && cmake --build build
   ./build/haywire --no-qemu                  # should open a window and stay up
   ```
   Needs `cmake glfw capstone` (Homebrew on macOS). A stale `build/` copied from another
   machine is fine to reuse if it's the same architecture; `cmake --build build --clean-first`
   proves the source actually compiles on the new host.

2. **Get QEMU, firmware and swtpm.** See [Package managers](#package-managers) — on an
   Intel Mac this is no longer just `brew install qemu`.

3. **Copy VM images only while the VMs are shut down** (on the source host), into `vms/`
   (gitignored). Then verify before booting anything — see [VM images](#vm-images).

4. **Adjust the launch scripts** for the host's accelerator and firmware paths. The
   scripts detect Apple Silicon vs Intel with `sysctl -n hw.optional.arm64` (not
   `uname -m`, which reports x86_64 under Rosetta).

5. **Boot the VM, then run Haywire against it**
   ```bash
   ./scripts/launch_windows_x86_64_macos.sh
   ./build/haywire --guest-os windows         # in another terminal
   ```
   Success looks like: both RAM regions (`ram-below-4g`, `ram-above-4g`) discovered via the
   monitor on :4444, and the blind scan finding ~100+ processes with System's DTB matching
   the guest's CR3.

## Accelerator matrix

| Guest | Host | Accelerator | Notes |
|---|---|---|---|
| Linux ARM64 | Apple Silicon | HVF, `-cpu host` | fast |
| Linux ARM64 | Intel Mac | **TCG**, `-cpu max` | HVF can't run a foreign ISA; slow |
| Windows 11 x86_64 | Apple Silicon | TCG, `-cpu max`, `-vga std` | slow but stable |
| Windows 11 x86_64 | Intel Mac | HVF — needs the specific settings below | fast |
| Windows 11 x86_64 | Linux / WSL2 | KVM, `-cpu host,-vmx,-hypervisor,+invtsc` | see `docs/windows11_vm_setup.md` |

## Windows 11 under QEMU 11.1 HVF on an Intel Mac

Every one of these was hit in order. The working configuration is in
`scripts/launch_windows_x86_64_macos.sh` (HVF branch).

| Symptom | Cause | Fix |
|---|---|---|
| QEMU aborts at startup: `hvf-all.c:83:do_hv_vm_protect: assertion failed: (!(size & ~page_mask))` | VGA dirty tracking calls `hv_vm_protect` on the visible framebuffer size, which must be whole 4K pages. 800×600×4 = 0x1d4c00 isn't. Only happens with a real display (Cocoa, or VNC once a client connects) — `-display none` hides it. | Pin a page-aligned mode: `-vga none -device VGA,edid=on,xres=1280,yres=800` |
| Still crashes at 800×600 despite the EDID mode | `firmware/OVMF_CODE.fd` is edk2-stable202011 and ignores EDID | Use the host QEMU's newer `edk2-x86_64-code.fd` (same size/VARS layout; existing VARS keep their boot entries) |
| Firmware logo shows, then "Display output is not active" / frozen boot screen once Windows loads | ramfb and virtio-gpu-pci avoid the assert, but x86 Windows ignores a non-PCI framebuffer (ramfb), and virtio-gpu needs the `viogpudo` driver to scan out | Use VGA as above |
| Black screen, firmware spins, disk never read | Any TPM device (`tpm-tis` or `tpm-crb`) adds a 0x400-byte PPI RAM region at 0xfed45000. HVF can't map sub-page RAM, so edk2 hangs at `[TPM2PP] mPpi=FED45000`. Also makes Windows *reboots* hang in the boot manager. QEMU 11.1 removed `ppi=off`. | Run without a TPM under HVF. Windows 11 boots fine once installed; BitLocker didn't prompt. `vms/swtpm_win11/` is kept for TCG hosts. |
| "Automatic Repair — Your PC did not start correctly" | Guest sees VT-x, tries to start its own hypervisor (VBS/WSL2), HVF can't nest it. Same failure as the WSL2/KVM setup. | `-vmx` and `-hypervisor` on the CPU |
| `IRQL_NOT_LESS_OR_EQUAL (0xA)`, `ntoskrnl.exe`, ~90 s after the lock screen, repeatedly | `-cpu host` under HVF. Reproduced with and without `+invtsc`, with `-smp 1`, and without the memory-backend-file — so not SMP, TSC, or Haywire's shared RAM. QEMU warns HVF lacks `tsc-deadline invpcid spec-ctrl xsavec`. | `-cpu Skylake-Client-noTSX-IBRS,-hypervisor` — 15-minute soak with 4 vCPUs/8G, zero resets |
| Windows clock off by the UTC offset | QEMU's RTC is UTC; Windows reads it as local time | `-rtc base=localtime` |
| ACPI power button restarts instead of shutting down | Seen once after a failed boot; don't rely on it | Shut down from inside Windows; `system_powerdown` via QMP as a fallback, `quit` only when the guest is in firmware/boot manager |

Not yet revisited: `scripts/launch_windows_x86_64_macos_nonet.sh` still uses the old
TCG/`-vga std` settings.

## Debugging techniques that found these

All of these work without seeing the QEMU window, and without risking the real disk.

- **Is the guest in firmware or the kernel?** QMP `human-monitor-command` → `info registers -a`.
  RIP `0x7e……` = UEFI/boot manager; `0xfffff80……` = Windows kernel. Busy CPU + halted APs
  at a fixed firmware RIP = firmware hang.
- **What's on screen?** QMP `screendump` to a `.ppm`, convert with `sips -s format png`.
  An all-zero frame is a blanked display; a crash screen has text.
- **Is it touching the disk?** QMP `query-blockstats` twice, a few seconds apart.
- **Firmware log:** the host edk2 is a debug build. Add
  `-debugcon file:dbg.log -global isa-debugcon.iobase=0x402`; the last line shows where it hung.
- **HVF memory mapping:** `-trace 'hvf_vm_*'` shows every map/unmap/protect with its size —
  that's how the 0x1d4c00 framebuffer and 0x400 TPM region were found.
- **Throwaway boots of the real disk:** `cp -c` (APFS clone — instant, no extra space)
  the qcow2, then boot the clone with `-drive …,snapshot=on` and a copy of the VARS file.
  Nothing reaches the real image, and several variants can run in parallel (32 GB RAM
  handled three 4 GB guests plus the real VM). A script that boots, screendumps every 30 s,
  and logs QMP `RESET` events turned "it crashes sometimes" into a 6-minute A/B test.
- **Read QEMU's source for an assertion:**
  `curl -sL https://gitlab.com/qemu-project/qemu/-/raw/v<version>/accel/hvf/hvf-all.c`.

## VM images

- Copy images only while the VM is shut down on the source host.
- Check before booting: `qemu-img check vms/<image>.qcow2` (read-only without `-r`).
  Also `qemu-img snapshot -l` — **`windows11.qcow2` has internal snapshots**
  (`clean_install`, `working_with_net`).
- **Never run `qemu-img check -r leaks` on an image with internal snapshots in place.**
  It classified ~444k snapshot-shared clusters as leaks and dropped their refcounts,
  producing 159,536 `refcount=1 reference=2` errors. `-r all` on an APFS clone restored
  consistency (0 errors, 0 leaks) and the clone was swapped in. Leaks are harmless
  ("waste of disk space, but no harm to data") — leaving them is a fine choice.
- Do any repair on a `cp -c` clone first; keep the source host's copy until the VM has
  booted and shut down cleanly on the new host.
- Hard-stopping a Windows guest mid-boot counts as a failed start and pushes it toward
  Automatic Repair.

## Package managers

- **Homebrew dropped Intel macOS (September 2026).** No bottles; everything builds from
  source and it nags about Command Line Tools. Its qemu 11.1.2 **fails to build**: QEMU's
  configure pip-installs Python wheels and Homebrew's build sandbox has no network.
- **QEMU from MacPorts** (binary for Sequoia/Intel): `sudo port install qemu`. Firmware is in
  `/opt/local/share/qemu/`. The scripts search `brew --prefix`, `/opt/homebrew`,
  `/usr/local`, `/opt/local`. Installing via `sudo installer -pkg …` from the command line
  does **not** add `/opt/local/bin` to PATH — add it to `~/.zprofile` (after Homebrew's
  paths so Homebrew keeps priority).
- **swtpm isn't in MacPorts;** Homebrew's builds, but its `openssl@4` dependency deadlocks:
  linking openssl@4 wants to unlink openssl@3, which the same install holds a lock on
  (`A brew install … process has already locked /usr/local/Cellar/openssl@3`). It leaves a
  keg with no `opt/` link, so every later install tries to rebuild it (`Errno::EEXIST … share/man`).
  Workaround: build openssl@4 once, remove any `4.0.3.tmp` leftover, then
  `ln -s ../Cellar/openssl@4/<ver> /usr/local/opt/openssl@4` and `brew install swtpm`.
  openssl@3 stays the linked default.
- **Set `HOMEBREW_NO_AUTOREMOVE=1` before any `brew uninstall`.** Without it, uninstalling
  one broken keg also removed seven "unused" packages (including gawk and ncurses).
- Diff `brew list --formula` before and after risky brew operations.

## Host environment

- **Check the login shell** (`dscl . -read ~ UserShell`). This Mac switched bash→zsh on
  arrival (the Claude Code installer's instructions led to a `chsh`), silently dropping
  `~/.bashrc` aliases and `~/.bash_profile` PATH entries. Aliases were ported to `~/.zshrc`;
  PATH entries to `~/.zprofile`. Losing the python.org 3.12 PATH entry made `pip3` resolve
  to Homebrew's Python 3.11 while `python3` stayed 3.12.
- **GitHub over SSH:** put the key in `~/.ssh`, add a `Host github.com` block with
  `IdentityFile` and `IdentitiesOnly yes`, verify with `ssh -T git@github.com`, then
  `git remote set-url origin git@github.com:jamiefaye/haywire.git`. Submodules can stay HTTPS.
  Check any copied private key isn't world-readable (`chmod 600`).
