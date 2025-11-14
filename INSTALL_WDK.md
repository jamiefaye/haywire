# Install Windows Driver Kit (WDK) for VirtualBox Build

VirtualBox needs the Windows Driver Kit to build kernel drivers (vboxdrv.sys, etc.)

## Quick Install

**Download WDK:**
https://go.microsoft.com/fwlink/?linkid=2249371

This is the **WDK for Windows 11, version 22H2** (works for Windows 10 too)

**What it includes:**
- Windows 10 SDK (required)
- Windows Driver Kit (WDK)
- Everything needed to build kernel drivers

**Installation:**
1. Run the installer
2. Install to default location: `C:\Program Files (x86)\Windows Kits\10`
3. Takes ~10-15 minutes
4. Requires ~3 GB disk space

## After Installing

Run the configure script again:

```cmd
cd C:\Users\jamie\vbox-src
configure_vbox.cmd
```

It should now find the WDK automatically.

## Alternative: Use EWDK (Enterprise WDK)

If you want a standalone version that doesn't require installation:

**Download EWDK:**
https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk

- Download the ISO
- Mount it (right-click → Mount)
- Point configure to it: `--with-DDK=D:\` (where D: is your mounted drive)

## After WDK is Installed

The configure should complete and create:
- `env.bat` - Build environment
- `AutoConfig.kmk` - Build configuration
- `out/` - Output directory

Then you can build VirtualBox!

---

**For now: Just download and install the WDK from the first link above, then re-run configure_vbox.cmd** ✅
