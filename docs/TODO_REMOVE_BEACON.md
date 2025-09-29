# TODO: Remove Beacon Dependencies

The beacon/companion system has been replaced by kernel discovery, but many files still have dependencies on the old system. This needs to be cleaned up.

## Files that include beacon headers:

1. **main.cpp** - includes beacon_reader.h
2. **pid_selector.cpp** - includes beacon_reader.h
3. **memory_utils.cpp** - includes beacon_reader.h
4. **bitmap_viewer.cpp** - includes beacon_reader.h
5. **crunched_memory_reader.cpp** - includes beacon_translator.h
6. **memory_visualizer.cpp** - includes crunched_memory_reader.h (which includes beacon_translator.h)
7. **qemu_memory_source.cpp** - includes crunched_memory_reader.h
8. **kernel_discovery_backend.cpp** - includes beacon_decoder.h for SectionEntry definition

## What needs to be done:

1. **Move SectionEntry definition** from beacon_decoder.h to a new header (e.g., memory_types.h)
2. **Remove beacon reader usage** from main.cpp - it's already using kernel_discovery_backend
3. **Update pid_selector** to use kernel_discovery_backend instead of beacon_reader
4. **Update memory_utils** to remove beacon dependencies
5. **Update bitmap_viewer** to remove beacon dependencies
6. **Remove beacon_translator** from crunched_memory_reader - use kernel_discovery for VA translation
7. **Update all visualization code** to use kernel discovery

## Current state:

- Beacon files are in `obsolete/` directory
- Headers and source files temporarily restored to keep build working
- Kernel discovery is functional and provides all needed functionality

## Replacement mapping:

- BeaconReader → KernelDiscoveryBackend
- BeaconTranslator → KernelDiscoveryBackend (TranslateVA method)
- SectionEntry → Move to common header
- Process discovery → KernelDiscoveryBackend::GetPIDList
- Memory sections → KernelDiscoveryBackend::GetProcessSections