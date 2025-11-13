# Install WDK 7.1 - Minimal (Skip .NET Components)

The WDK installer hangs on .NET downloads. Here's how to install just what VirtualBox needs:

## Method 1: Cancel and Try Offline Install

1. **Cancel the current installation** (the one stuck at 60%)
2. **Remount the ISO** (right-click → Mount)
3. **Run the installer again**
4. When it starts, **uncheck** any optional components:
   - Uncheck ".NET Framework"
   - Uncheck "Documentation"
   - Uncheck "Samples"
   - **Keep checked**: "Build Environments", "Tools"
5. Install to: `C:\WinDDK\7600.16385.1`

## Method 2: Extract Manually (If Installer Keeps Failing)

If the installer keeps hanging, we can extract just the files VirtualBox needs:

1. Download 7-Zip: https://www.7-zip.org/download.html
2. Right-click the ISO → 7-Zip → Extract to folder
3. Look for the MSI files inside
4. Extract the main WDK MSI to `C:\WinDDK\7600.16385.1`

## Method 3: Use Pre-extracted Version

Someone may have uploaded a pre-extracted version. Check:
- Archive.org for "WDK 7.1 extracted"
- Or use the VirtualBox tools directory approach (see below)

## Quick Workaround: Tell VirtualBox to Skip DDK

Actually, since we're only patching the memory allocation code (which is in user-mode PGM.cpp and kernel memobj), we might be able to skip the full DDK.

Try adding this to configure:
```cmd
cscript //Nologo configure.vbs --with-SDK10="C:\Program Files (x86)\Windows Kits\10" --continue-on-error
```

The `--continue-on-error` flag might let it proceed with warnings.

---

**Recommendation:** Try Method 1 first (uncheck .NET and optional components). The build environment and tools are all we need - the .NET stuff is for sample applications we don't care about.
