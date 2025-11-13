# Download Old Windows DDK v7.1 for VirtualBox

VirtualBox 7.1 source still requires the old Windows DDK v7.1 (Windows 7 era).

## Quick Solution

**Download DDK 7.1:**
https://go.microsoft.com/fwlink/?LinkId=321800

This is the official Microsoft link for WDK 7.1 (also called DDK 7600.16385.1)

**Install it:**
1. Run the installer
2. Install to: `C:\WinDDK\7600.16385.1\`
3. Takes ~5-10 minutes

**Then tell configure where it is:**

Edit `C:\Users\jamie\vbox-src\configure_vbox.cmd` and change the configure line to:

```cmd
cscript //Nologo configure.vbs --with-DDK="C:\WinDDK\7600.16385.1"
```

## Alternative: Download Pre-extracted

If the installer doesn't work, you can download a pre-extracted version:
- Search for "Windows DDK 7600.16385.1 download"
- Extract to `C:\Users\jamie\vbox-src\tools\win.x86\ddk\7600.16385.1`

This is where configure is looking for it by default.

## Why the Old DDK?

VirtualBox still uses some legacy driver APIs that were removed in modern WDK versions. They maintain compatibility with older Windows versions (Vista, 7) which require the old DDK.

The DDK you installed earlier (modern WDK) is also needed - VirtualBox uses both!

---

**Try downloading and installing DDK 7.1 from the link above, then re-run configure_vbox.cmd**
