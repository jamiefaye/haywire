# VirtualBox Build Environment Setup Guide

## Quick Start (Automated)

**Just run this:**

```cmd
cd C:\Users\jamie\haywire
setup_vbox_build_env.cmd
```

This script will:
1. Check for Visual Studio, Python, Windows SDK
2. Download and install kBuild automatically
3. Configure the VirtualBox source tree
4. Create helper scripts for building
5. Verify everything is ready

**Time:** 30-60 minutes (mostly downloads and configure)

---

## Manual Steps (If Script Fails)

### 1. Install Prerequisites

**Already Have:**
- ✅ Visual Studio 2022 (detected by test_vbox_com.cpp compilation)
- ✅ Python 3
- ✅ VirtualBox source at `C:\Users\jamie\vbox-src`

**May Need:**
- Windows 10 SDK (usually installed with VS, check "Windows 10 SDK" in VS Installer)
- Windows DDK/WDK (for driver compilation)

### 2. Install kBuild

kBuild is VirtualBox's custom build system (like Make, but for VirtualBox).

**Download:**
https://download.virtualbox.org/virtualbox/7.0.14/kBuild-0.1.9998-r3592-win-amd64.exe

**Install to:** `C:\kBuild` (default)

**Add to PATH:**
```cmd
set PATH=C:\kBuild\bin;%PATH%
```

### 3. Configure VirtualBox Source

```cmd
cd C:\Users\jamie\vbox-src

REM Set up Visual Studio environment
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

REM Configure (disable optional components we don't need)
cscript //Nologo configure.vbs --with-MinGW-w64= --with-libSDL= --with-openssl= --with-qt5= --with-python=
```

This creates:
- `env.bat` - Environment setup script
- `AutoConfig.kmk` - Build configuration
- `out/` - Build output directory

### 4. Build VirtualBox

```cmd
cd C:\Users\jamie\vbox-src

REM Load environment
call env.bat

REM Build
cd out\win.amd64\release
kmk
```

**First build:** 2-4 hours
**Incremental builds:** 10-30 minutes

---

## What Gets Built

After successful build, you'll have:

```
C:\Users\jamie\vbox-src\out\win.amd64\release\
├── bin/
│   ├── VirtualBox.exe          # Main application
│   ├── VBoxManage.exe          # Command-line tool
│   ├── VBoxSVC.exe             # Service process
│   └── ...
├── drivers/
│   ├── VBoxDrv/
│   │   └── VBoxDrv.sys         # Main kernel driver (we'll patch this!)
│   ├── VBoxUSB/
│   └── VBoxNetAdp/
└── ...
```

---

## Where We'll Add Our Patch

The patch goes in **TWO** places:

### 1. User-Mode: Create Shared Memory File

**File:** `src/VBox/VMM/VMMR3/PGM.cpp`

This runs in `VBoxSVC.exe` and creates the memory-mapped file that Haywire will read.

### 2. Kernel-Mode: Allocate from Shared Memory

**File:** `src/VBox/Runtime/r0drv/nt/memobj-r0drv-nt.cpp`

This runs in `VBoxDrv.sys` and redirects chunk allocations to our shared memory.

---

## Testing the Build

Before patching, let's verify the build works:

### 1. Uninstall Current VirtualBox

```cmd
"C:\Program Files\Oracle\VirtualBox\uninst.exe"
```

### 2. Install Your Build

```cmd
cd C:\Users\jamie\vbox-src\out\win.amd64\release\bin
VirtualBox.exe
```

Should launch VirtualBox with your compiled version.

### 3. Test Your VM

```cmd
cd C:\Users\jamie\vbox-src\out\win.amd64\release\bin
VBoxManage startvm "Ubuntu-x86_64-Haywire" --type headless
```

If this works, your build environment is perfect!

---

## Build Troubleshooting

### Error: "kmk: command not found"

kBuild not in PATH. Fix:
```cmd
set PATH=C:\kBuild\bin;%PATH%
```

Or run from VirtualBox build environment:
```cmd
C:\Users\jamie\vbox-src\buildenv.cmd
```

### Error: "MSVC compiler not found"

Visual Studio environment not set up. Fix:
```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

### Error: "SDK not found"

Missing Windows SDK. Install via Visual Studio Installer:
- Open Visual Studio Installer
- Modify VS 2022
- Check "Windows 10 SDK (10.0.xxxxx)"

### Build is Very Slow

First build compiles everything (2-4 hours). Subsequent builds are much faster.

**Speed up:** Use parallel build
```cmd
kmk -j8  # Use 8 parallel jobs
```

### Driver Signing Errors

VirtualBox drivers must be signed. The build should:
1. Create test certificates automatically
2. Sign drivers with test cert
3. Install test cert to Trusted Publishers

If signing fails, you may need to enable Test Mode:
```cmd
bcdedit /set testsigning on
REM Reboot required
```

---

## After Build: Apply the Patch

Once you have a working build, we'll:

1. **Add shared memory code** to PGM.cpp
2. **Hook allocations** in memobj-r0drv-nt.cpp
3. **Rebuild** (only affected files, ~10 minutes)
4. **Test** with Haywire

---

## Build Environment Shortcuts

The setup script creates these helpers:

**Open Build Environment:**
```cmd
C:\Users\jamie\vbox-src\buildenv.cmd
```

**Quick Build:**
```cmd
C:\Users\jamie\vbox-src\quick_build.cmd
```

**Rebuild After Changes:**
```cmd
cd C:\Users\jamie\vbox-src
call env.bat
cd out\win.amd64\release
kmk
```

---

## Disk Space Requirements

- **Source:** ~1.5 GB
- **Build output:** ~3-4 GB
- **Total:** ~5-6 GB

Make sure you have at least **10 GB free** to be safe.

---

## Next Steps

1. **Run setup script:** `setup_vbox_build_env.cmd`
2. **Wait for completion** (30-60 min)
3. **Verify build works** (test VM boots)
4. **Apply memory patch** (next phase)
5. **Rebuild and test** with Haywire

**Ready to run the setup script?** It will handle everything automatically with minimal prompts!
