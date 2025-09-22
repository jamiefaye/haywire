# Inspector Fix - Physical Address Mode

## Problem
The inspector (magnifier window with hex display) was showing zeros instead of actual memory content when searching for strings like "vlc". This occurred when VA (Virtual Address) mode was enabled.

## Root Cause
1. When selecting a process PID, the code auto-enabled VA mode
2. In VA mode, the search stores flattened/virtual addresses in searchResults
3. The inspector's hex display couldn't properly translate these addresses back to physical memory
4. The inspector reads from `/tmp/haywire-vm-mem` which uses physical addresses (0-6GB range)

## Solution
Disabled automatic VA mode when selecting a PID by commenting out lines 121-132 in memory_visualizer.cpp. The inspector now works in PA (Physical Address) mode only.

## Files Modified
- `src/memory_visualizer.cpp`: Commented out auto-enable of VA mode

## Limitations
- VA mode is temporarily disabled for the inspector to work correctly
- Search and hex display only work with physical addresses (0-6GB range)
- Future work needed: Fix inspector to properly handle VA mode address translation

## Testing
1. Start HWC++ 
2. Search for ASCII string (e.g., "vlc")
3. Inspector should now show the actual hex data instead of zeros
4. Navigation through search results should display correct memory content
