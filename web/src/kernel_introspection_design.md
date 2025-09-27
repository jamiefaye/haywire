# Kernel Memory Introspection - Web/Electron Integration

## Overview
Integrate kernel structure discovery into Haywire's web UI to provide complete VM introspection without companion programs.

## Discovered Kernel Structures

### 1. Master Page Table
- **swapper_pg_dir**: `0x082c00000` (physical)
- Controls all kernel virtual → physical translations
- 255 kernel space entries

### 2. Page Table Hierarchy
```
swapper_pg_dir (0x082c00000)
    ├── PMD Tables (27 found)
    │   ├── 0x11bd3e000 → 17 PTEs
    │   ├── 0x12a043000 → 10 PTEs
    │   └── ...
    └── PTE Tables (272 found)
        ├── Cluster @ 330MB
        ├── Cluster @ 770MB
        ├── Cluster @ 880-920MB
        └── Cluster @ 3.75GB
```

### 3. Process List
- **1,010 task_structs** found
- Each contains: PID, process name, memory mappings
- Located at SLAB offsets: 0x0, 0x2380, 0x4700

## UI Components Design

### A. Process Explorer View
```vue
<ProcessExplorer>
  ├── Process List Panel
  │   ├── PID | Name | Memory | Status
  │   ├── [1] init
  │   ├── [2] kthreadd
  │   └── ... (1,010 processes)
  │
  └── Memory Map Panel
      ├── Virtual Address | Physical | Size | Permissions
      └── Page Table Viewer
```

### B. Kernel Memory Navigator
```vue
<KernelNavigator>
  ├── Address Bar
  │   └── [0xffff800000000000] [Translate] [Follow]
  │
  ├── Page Table Tree
  │   ├── PGD[256-511] (kernel space)
  │   ├── → PMD Tables
  │   └── → → PTE Tables
  │
  └── Memory Display
      └── Hex/ASCII view of translated addresses
```

### C. Live Memory Search
```vue
<MemorySearch>
  ├── Search Bar
  │   └── [Search pattern] [x] Kernel [x] Physical [x] Virtual
  │
  └── Results
      ├── Found at 0x082c00000 (swapper_pg_dir)
      ├── Found at 0x11bd3e000 (PMD table)
      └── ...
```

## Implementation Architecture

### 1. C++ Backend (`src/kernel_discovery.cpp`)
```cpp
class KernelDiscovery {
public:
    struct KernelStructures {
        uint64_t swapper_pg_dir;
        vector<PMDTable> pmd_tables;
        vector<PTETable> pte_tables;
        vector<TaskStruct> processes;
    };

    KernelStructures discover(void* mmap_base, size_t size);
    uint64_t translateVirtual(uint64_t vaddr);
    ProcessInfo* findProcess(int pid);

private:
    bool checkTaskStruct(void* addr);
    bool checkPTETable(void* addr);
    bool checkPMDTable(void* addr);
    bool checkPGD(void* addr);
};
```

### 2. WebAssembly Bridge (`web/src/wasm/kernel_discovery.js`)
```javascript
// Compiled from C++ to WASM
Module.KernelDiscovery = {
    discover: function(memoryBuffer) {
        // Returns discovered structures
        return {
            swapperPgDir: 0x082c00000,
            pmdTables: [...],
            pteTables: [...],
            processes: [...]
        };
    },

    translateAddress: function(virtualAddr, pgdAddr) {
        // Walk page tables to translate
        // 0xffff800000000000 → 0x40000000
    },

    findProcess: function(pid) {
        // Return task_struct info
        return {
            pid: pid,
            comm: "systemd",
            mm: 0x...,
            pgd: 0x...
        };
    }
};
```

### 3. Vue Components (`web/src/components/`)

#### ProcessList.vue
```vue
<template>
  <div class="process-list">
    <div v-for="proc in processes" :key="proc.pid">
      <span>{{ proc.pid }}</span>
      <span>{{ proc.comm }}</span>
      <button @click="viewMemory(proc)">View Memory</button>
    </div>
  </div>
</template>

<script>
export default {
    data() {
        return {
            processes: []
        };
    },
    mounted() {
        // Discover kernel structures on load
        this.processes = Module.KernelDiscovery.discover().processes;
    },
    methods: {
        viewMemory(proc) {
            // Navigate to process memory view
            this.$emit('select-process', proc);
        }
    }
};
</script>
```

#### PageTableViewer.vue
```vue
<template>
  <div class="page-table-viewer">
    <div class="pgd-view">
      <h3>PGD @ {{ formatAddr(pgdAddress) }}</h3>
      <div v-for="(entry, idx) in pgdEntries"
           v-if="idx >= 256">
        PGD[{{ idx }}]: {{ formatEntry(entry) }}
      </div>
    </div>
  </div>
</template>
```

#### KernelMemoryView.vue
```vue
<template>
  <div class="kernel-memory">
    <AddressBar v-model="currentAddress"
                @translate="translateAddress" />
    <MemoryHexView :address="physicalAddress"
                   :data="memoryData" />
  </div>
</template>
```

## Data Flow

```
1. Open memory file → WASM discovers structures
                          ↓
2. KernelDiscovery.discover() → {pgd, pmds, ptes, tasks}
                          ↓
3. Vue components display → Process List
                          → Page Tables
                          → Memory Views
                          ↓
4. User selects process → Walk page tables
                          ↓
5. Translate VA to PA → Display memory
```

## Key Features

### 1. Auto-Discovery
- On file load, automatically find kernel structures
- No need for symbols or debug info
- Works with any Linux ARM64 VM

### 2. Live Translation
- Enter any kernel virtual address (0xffff...)
- Walk page tables to find physical address
- Display actual memory contents

### 3. Process Memory Mapping
- Select any process from discovered list
- View its page table (task_struct→mm→pgd)
- See all memory mappings

### 4. Search Capabilities
- Search for patterns in physical memory
- Search in specific virtual address ranges
- Find kernel symbols by pattern

## Performance Optimizations

### 1. Lazy Loading
- Don't scan entire 6GB upfront
- Discover structures on-demand
- Cache discovered tables

### 2. WASM SIMD
- Use SIMD for pattern matching
- Parallel search across memory
- 4-8x speedup vs JavaScript

### 3. Incremental Discovery
- Start with swapper_pg_dir region
- Expand search as needed
- Progressive enhancement

## Integration Steps

1. **Port Python to C++**
   - Convert `kernel_discovery_complete.py`
   - Optimize for performance
   - Add to existing Haywire codebase

2. **Compile to WASM**
   - Use Emscripten with SIMD
   - Export discovery functions
   - Test with sample memory files

3. **Create Vue Components**
   - ProcessList.vue
   - PageTableViewer.vue
   - KernelMemoryView.vue

4. **Wire Up Data Flow**
   - Connect WASM to Vue
   - Add to existing memory viewer
   - Test with live VMs

## UI Mockup

```
┌─────────────────────────────────────────────────────────┐
│ Haywire - Kernel Memory Introspection                  │
├─────────────────┬───────────────────────────────────────┤
│ Processes (1010)│ Memory View                          │
├─────────────────┼───────────────────────────────────────┤
│ PID  Name       │ Virtual: 0xffff800000000000          │
│ 1    systemd    │ Physical: 0x082c00000                 │
│ 2    kthreadd   │                                       │
│ 3    rcu_gp     │ 00000000: 4b 4b 4b 4b 4b 4b 4b 4b  │
│ ...             │ 00000008: 4b 4b 4b 4b 4b 4b 4b 4b  │
│                 │ 00000010: 4b 4b 4b 4b 4b 4b 4b 4b  │
├─────────────────┼───────────────────────────────────────┤
│ Page Tables     │ [Translate] [Search] [Export]        │
│ ├─ PGD @82c0000 │                                       │
│ │  ├─ [256]: OK │ Status: swapper_pg_dir found         │
│ │  ├─ [257]: OK │ 27 PMDs, 272 PTEs discovered         │
│ │  └─ ...       │                                       │
└─────────────────┴───────────────────────────────────────┘
```

## Success Metrics

✓ Auto-discover kernel structures in < 5 seconds
✓ Display all 1000+ processes with names
✓ Translate any kernel virtual address
✓ Navigate page table hierarchy visually
✓ Search memory by pattern or address

## Next Steps

1. Create `src/kernel_discovery.cpp`
2. Build WASM module
3. Create Vue components
4. Integrate with existing Haywire UI
5. Test with various kernel versions