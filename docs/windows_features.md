# Windows-Specific Features for Haywire

## Status: Planning / Future Implementation

This document tracks Windows-specific features needed for full Windows 11 introspection support.

## Current Status (Nov 7, 2025)

### Working
- ✅ Windows 11 VM setup with QEMU
- ✅ Memory backend file accessible at `/tmp/haywire-vm-mem`
- ✅ QMP connection on port 4445
- ✅ Raw memory visualization (8GB accessible)
- ✅ Physical address mode works perfectly

### Not Yet Implemented
- ❌ Windows kernel discovery (EPROCESS scanning)
- ❌ Virtual address translation (Windows page tables)
- ❌ Process selection/filtering

## Feature: UTF-16 Character Display

### Motivation
Windows uses UTF-16 (wide characters) extensively for all string data:
- Process names in EPROCESS structures
- File paths (all Windows paths are UTF-16)
- Registry strings
- Window titles and UI text
- Command line arguments

### Current Limitation
The existing "Char 8-bit" format displays single bytes as ASCII/Latin-1 characters using the 6x8 pixel font. This works for ASCII but cannot display Windows UTF-16 strings correctly.

### Proposed Implementation

Add a new pixel format: **"Char 16-bit"** or **"Unicode"**

**Format Details:**
- Read 2 bytes per character (little-endian)
- Use same 6x8 pixel font
- Map UTF-16 code points to glyphs
- For unmapped characters, display '?' or box

**Use Cases:**
1. Visual scanning for process names ("System", "csrss.exe", "explorer.exe")
2. Locating EPROCESS structures by finding recognizable strings
3. Finding file paths in memory
4. Debugging Unicode string handling

**Implementation Files:**
- `include/pixel_format.h` - add new format enum
- `src/memory_visualizer.cpp` - add rendering case
- Font already available: 6x8 bitmap with Unicode coverage

**Rendering Logic:**
```cpp
case PixelFormat::Char16:
    for (int i = 0; i < width; i++) {
        if (offset + 1 >= size) break;
        uint16_t wchar = data[offset] | (data[offset+1] << 8);
        offset += 2;
        // Map wchar to 6x8 glyph
        RenderGlyph(wchar, x + i*6, y);
    }
```

### Priority
**Medium** - Very useful for Windows process discovery, but raw memory view works for now.

## Feature: Windows Kernel Discovery

### Status
Not yet implemented - infrastructure exists but needs Windows-specific implementation.

### Requirements

1. **EPROCESS Scanner**
   - Scan memory for EPROCESS structures
   - Validate with known signatures/patterns
   - Extract process information

2. **Windows Kernel Profile**
   - EPROCESS structure offsets
   - KPROCESS offsets
   - Page table formats (x86-64 specific)
   - System process identification

3. **Page Table Walking**
   - CR3/DirectoryTableBase from EPROCESS
   - 4-level page tables (PML4 → PDPT → PD → PT)
   - Handle large pages (2MB, 1GB)
   - Virtual address translation

### Reference Resources
- Vergilius Project: https://www.vergiliusproject.com/
- Windows Internals book
- WinDbg kernel debugging documentation

### Priority
**High** - Core functionality for Windows introspection

## Feature: Windows Kernel Profile System

Similar to Linux profiles (`profiles/linux/`), create Windows profiles:

```
profiles/windows/
  win11-26100-x64.json     # Windows 11 25H2
  win10-19045-x64.json     # Windows 10 22H2
```

Profile contents:
- EPROCESS offsets (ImageFileName, UniqueProcessId, ActiveProcessLinks, etc.)
- KPROCESS offsets (DirectoryTableBase)
- PEB offsets (ProcessParameters, ImagePathName)
- Kernel version detection strings

### Priority
**High** - Needed for multi-version support

## Feature: Process Tree Visualization

Once EPROCESS scanning works, show process hierarchy based on InheritedFromUniqueProcessId.

### Priority
**Low** - Nice to have, not critical

