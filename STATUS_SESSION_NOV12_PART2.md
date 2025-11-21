# Windows Guest Process Discovery - November 12, 2025 Part 2

## Major Fixes Implemented

### 1. ✅ CR3/DTB Flag Masking
**Problem**: Windows DirectoryTableBase values have flag bits in positions 0-11 and 52-63
**Example**: Raw value `0x4b424742` (ASCII "BGBK") → masked to `0x4b424000`

**Fixed in:**
- `IsValidEPROCESS()` - validation before accepting EPROCESS structure
- `FindAllSystemProcesses()` - System process scanning
- `DiscoverProcesses()` - DTB extraction from found processes
- `ExtractProcessPGDs()` - final DTB assignment
- `TranslateVA()` - all PML4/PDPT/PD/PT address extraction

**Mask used**: `0x000FFFFFFFFFF000ULL` (bits 12-51 only)

### 2. ✅ Fixed ImageFileName Offset
**Problem**: Reading at offset 1448, but actual offset is 824 for Windows 11 Build 26100
**Result**: Process names were cut off or showed garbage

**Fix**: Updated `profiles/windows/windows-11-26100-x86_64.json`:
```json
"ImageFileName": {
  "offset": 824,  // Was 1448
  ...
}
```

**Source**: Verified via Vergilius Project (https://www.vergiliusproject.com/kernels/x64/windows-11/24h2/_EPROCESS)

### 3. ✅ PTE Address Masking in TranslateVA
**Problem**: Page table entries also have flags in high bits, causing addresses like `0x482deb00000901f0` (14TB!)

**Fixed**: All PTE address extractions now use `0x000FFFFFFFFFF000ULL` mask:
- PML4 entry → PDPT address
- PDPT entry → PD address
- PD entry → PT address
- PT entry → Physical page address

### 4. ✅ System Process Validation via PML4[256]
**Problem**: Blind scan found many false System candidates
**Solution**: Validate by checking PML4[256] entry (kernel text mapping)

**Logic**: Real System process must have:
- PID = 4
- Name = "System"
- DTB with valid PML4[256] entry pointing to PDPT within RAM

## Current Status

### What's Working
✅ DTB masking prevents alignment validation failures
✅ Process names display correctly (when found)
✅ PA (Physical Address) mode works
✅ Memory file access working (8GB at `/tmp/haywire-vm-mem`)
✅ QMP connection working (ports 4444/4445)

### What's Not Working
❌ System process discovery - No valid PID 4 found
❌ ActiveProcessLinks walking - Can't walk without valid System
❌ VA (Virtual Address) mode - DTBs from blind scan are invalid
❌ Complete process list - Blind scan finds ~20 processes with many false positives

### Key Findings

**Blind Scan Results:**
- Finds 20-23 processes via memory scanning
- Many false positives with corrupted names
- DTB validation shows most are garbage:
  - PID 257 (csrss.exe): DTB=0x6000 → PML4[0] not present
  - PID 600 (dwm.exe): DTB=0xe317000 → PDPT beyond 8GB RAM
  - PID 1322 (svchost.exe): DTB=0x16e7c0000 → PML4[0] not present

**Missing Processes:**
- mspaint.exe (user reported running, not found)
- Many system services
- Only finding ~20% of actual running processes

## Technical Insights

### x86-64 CR3/PTE Format
```
Bits 0-11:   Flags (PCID, cache control, etc.)
Bits 12-51:  Physical address (40-bit, 1TB addressable)
Bits 52-63:  Reserved/extended flags
```

**Critical Lesson**: Always mask these when extracting physical addresses from:
- CR3 (DirectoryTableBase)
- PML4 entries
- PDPT entries
- PD entries
- PT entries

### Windows EPROCESS Structure (Build 26100)
```
Offset 0x028 (40):   KPROCESS.DirectoryTableBase (CR3)
Offset 0x338 (824):  ImageFileName[15]
Offset 0x440 (1088): UniqueProcessId (PID)
Offset 0x448 (1096): ActiveProcessLinks (LIST_ENTRY)
```

## Next Steps

### Option A: Improve System Process Discovery
- Try scanning with different alignment (16-byte vs 8-byte)
- Test each found PID 4 by attempting VA translation
- Look for other System signatures (low PID, specific thread count)

### Option B: Find PsActiveProcessHead Symbol
- Would give direct access to process list head
- Requires kernel debugging symbols or pattern scanning
- More reliable than finding System

### Option C: Accept Limited Functionality
- PA mode works fine
- 20 processes is enough for basic introspection
- Focus on other features (memory visualization, heat maps, etc.)

### Option D: Enhance Validation
- Require multiple structure members to validate
- Check ActiveProcessLinks Flink/Blink consistency
- Verify DTB produces valid translations for known kernel addresses

## Files Modified
- `src/windows/windows_kernel_discovery.cpp`:
  - DTB masking in 5 locations
  - ImageFileName offset usage
  - PML4[256] validation in System scan
  - PTE masking in TranslateVA()

- `profiles/windows/windows-11-26100-x86_64.json`:
  - ImageFileName offset: 1448 → 824

## Recommendations

**Short term**: Accept current state as progress checkpoint
- 20+ processes found (improvement from 4-5)
- Names display correctly
- PA mode fully functional

**Medium term**: Implement Option B (find PsActiveProcessHead)
- More reliable than System process heuristics
- Standard approach used by forensics tools
- Would unlock ActiveProcessLinks walking

**Long term**: Consider hybrid approach
- Use both blind scanning AND list walking
- Merge and deduplicate results
- Provides resilience if one method fails
