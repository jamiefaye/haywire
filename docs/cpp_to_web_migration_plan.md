# C++ to Web Migration Plan

**Goal:** Sync all features from C++ native version to web version

**Last Updated:** October 30, 2025

## Status Overview

| Feature | C++ Status | Web Status | Priority | Effort |
|---------|-----------|------------|----------|--------|
| **Core Algorithms** |
| ARM64 Kernel Discovery | ✅ Complete | ✅ Complete | - | - |
| x86_64 Kernel Discovery | ✅ Complete | ❌ Missing | HIGH | 2-3 days |
| ARM64 Page Table Walker | ✅ Complete | ⚠️ Partial | MEDIUM | 2 days |
| x86_64 Page Table Walker | ✅ Complete | ❌ Missing | HIGH | 1-2 days |
| Process Discovery | ✅ Complete | ⚠️ Partial | MEDIUM | 2 days |
| **UI Components** |
| Memory Canvas | ✅ Complete | ✅ Complete | - | - |
| Mini Bitmap Viewers | ✅ Complete | ✅ Complete | - | - |
| AutoCorrelator | ✅ Complete | ✅ Complete | - | - |
| Magnifying Glass | ✅ Complete | ✅ Complete | - | - |
| Overview Pane | ✅ Complete | ✅ Complete | - | - |
| Heat Map | ✅ Complete | ❓ Unknown | MEDIUM | 3 days |
| Process Selector | ✅ Complete | ✅ Complete | - | - |
| Memory Maps View | ✅ Complete | ❓ Unknown | LOW | 2 days |
| **Advanced Features** |
| Change Detection | ✅ Complete | ❓ Unknown | MEDIUM | 3-4 days |
| Search (parallel) | ✅ Complete | ⚠️ Basic | MEDIUM | 2 days |
| Column Mode | ✅ Complete | ❓ Unknown | LOW | 1 day |
| Split Components | ✅ Complete | ❓ Unknown | LOW | 1 day |
| **Architecture** |
| Live VM Access (QMP) | ✅ QEMU only | ❌ N/A (web) | - | - |
| Dump File Support | ✅ Complete | ✅ Complete | - | - |
| Kernel Profiles | ✅ Complete | ❌ Missing | MEDIUM | 1-2 days |

## Detailed Migration Tasks

### 1. x86_64 Kernel Discovery (HIGH PRIORITY)

**C++ Source:** `src/kernel_discovery.cpp` (lines 1-500)

**Web Target:** `web/src/kernel-discovery-x86_64.ts` (new file)

**Key Functions to Port:**
```cpp
// From kernel_discovery.cpp
FindSwapperPGD()           → findSwapperPGD()
AnalyzeSwapperCandidate()  → analyzeSwapperCandidate()
FindTaskStructs()          → findTaskStructs()
CheckTaskStruct()          → checkTaskStruct()
ExtractProcessPGDs()       → extractProcessPGDs()
```

**Challenges:**
- Async file I/O (use PagedMemory)
- No direct memory pointers (use Uint8Array)
- TypeScript type safety

**Estimated Time:** 2-3 days

### 2. x86_64 Page Table Walker (HIGH PRIORITY)

**C++ Source:** `src/page_table_walker.cpp`

**Web Target:** `web/src/page-table-walker-x86_64.ts` (new file)

**Key Functions:**
```cpp
TranslateVA()     → translateVA()
Walk4Level()      → walk4Level()
GetPTE()          → getPTE()
```

**Estimated Time:** 1-2 days

### 3. Heat Map (MEDIUM PRIORITY)

**C++ Source:** `src/change_detector.cpp`, `src/heat_map_widget.cpp`

**Web Target:** `web/src/components/HeatMap.vue` (new file)

**Implementation:**
- Use Web Workers for background scanning
- Canvas-based visualization
- Color gradient (Green → Yellow → Orange → Red → Blue)

**Estimated Time:** 3 days

### 4. Enhanced Search (MEDIUM PRIORITY)

**C++ Source:** `src/memory_visualizer.cpp` (search methods)

**Web Target:** Enhance existing search in `MemoryCanvas.vue`

**Features:**
- Parallel search using Web Workers
- Progress reporting
- Result caching

**Estimated Time:** 2 days

### 5. Kernel Profiles (MEDIUM PRIORITY)

**C++ Source:** `src/kernel_profile_loader.cpp`, `profiles/*.json`

**Web Target:**
- `web/src/kernel-profile.ts` (new file)
- Copy `profiles/*.json` to `web/public/profiles/`

**Implementation:**
- Load JSON profiles via fetch()
- Support runtime profile selection
- Validate against loaded memory dump

**Estimated Time:** 1-2 days

## Implementation Strategy

### Approach 1: Feature-by-Feature (Recommended)

**Pros:**
- Each feature is complete before moving on
- Easier to test
- Incremental progress

**Cons:**
- Longer before "complete" state

**Timeline:** 3-4 weeks total

### Approach 2: Core-First

**Pros:**
- Gets critical algorithms done first
- Can release early version

**Cons:**
- UI lags behind capabilities

**Timeline:** 1 week core, 2 weeks UI

## Testing Strategy

### Unit Tests
```typescript
// web/tests/kernel-discovery-x86_64.test.ts
describe('x86_64 Kernel Discovery', () => {
  it('finds swapper PGD in test dump', async () => {
    const dump = await loadTestDump('ubuntu-x86_64.bin');
    const pgd = await findSwapperPGD(dump);
    expect(pgd).toBeGreaterThan(0);
  });
});
```

### Integration Tests
- Load VirtualBox dump (your current 4.2GB dump!)
- Run discovery
- Compare results to C++ version
- Verify PGD candidates match

## Tools and Automation

### Code Translation Helpers

**Create a C++ → TypeScript translator:**

```python
# scripts/cpp_to_ts.py
def translate_function(cpp_code):
    # Replace C++ types with TypeScript
    ts_code = cpp_code
    ts_code = ts_code.replace('uint64_t', 'bigint')
    ts_code = ts_code.replace('uint32_t', 'number')
    ts_code = ts_code.replace('uint8_t', 'number')
    ts_code = ts_code.replace('std::vector<', 'Array<')
    # ... more replacements
    return ts_code
```

### Diff Tool for Sync

```bash
# scripts/check_sync.sh
# Compare C++ changes to web implementation
diff -u <(grep "^function" src/kernel_discovery.cpp) \
        <(grep "^function" web/src/kernel-discovery-x86_64.ts)
```

## Recommended Next Steps

**This Week:**
1. ✅ VirtualBox dump working in web ← DONE
2. Port x86_64 kernel discovery (2-3 days)
3. Test with your 4.2GB dump

**Next Week:**
4. Port x86_64 page table walker (1-2 days)
5. Integrate with UI (ProcessExplorer)
6. Test VA mode with dump

**Week 3:**
7. Add heat map component
8. Enhanced search with workers
9. Kernel profile support

## Success Metrics

**Phase 1 Complete When:**
- ✓ Can load VirtualBox x86_64 dump
- ✓ Discovers swapper PGD
- ✓ Extracts process PGDs
- ✓ Matches C++ discovery results

**Phase 2 Complete When:**
- ✓ VA→PA translation works
- ✓ Can view process memory
- ✓ Memory maps display correctly

**Full Feature Parity When:**
- ✓ All C++ features available in web
- ✓ Performance comparable (within 2x)
- ✓ Tests pass for both versions

## Long-Term Maintenance

### Keep Both in Sync

**Strategy:**
1. Make algorithm changes in C++ first (easier to debug)
2. Port validated changes to TypeScript
3. Automated tests verify parity

**Documentation:**
- Document algorithm changes in both codebases
- Maintain this migration plan
- Update CLAUDE.md with web-specific notes

---

**Total Estimated Effort:** 15-20 days of focused work

**Parallelizable:** Some UI work can happen alongside algorithm porting
