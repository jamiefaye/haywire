#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include "page_database.h"
#include "guest_agent.h"

namespace Haywire {

/**
 * FilterCriteria - Specifies which pages to include in a view
 */
struct FilterCriteria {
    enum FilterType {
        NONE = 0,           // No filtering (all pages)
        BY_PID,             // Filter by process ID(s)
        BY_OWNERSHIP,       // Filter by ownership type(s)
        BY_ATTRIBUTION,     // Filter attributed vs unattributed
        BY_FLAGS,           // Filter by memory protection flags
        BY_ADDRESS_RANGE,   // Filter by virtual address range
        CUSTOM              // Custom filter function
    };

    FilterType type;

    // BY_PID parameters
    std::vector<uint32_t> pids;     // Empty = all PIDs
    bool includePid0;               // Include unattributed pages (pid=0)

    // BY_OWNERSHIP parameters
    std::vector<PageMetadata::OwnershipType> ownershipTypes;

    // BY_ATTRIBUTION parameters
    enum AttributionMode {
        ALL_PAGES,
        ATTRIBUTED_ONLY,
        UNATTRIBUTED_ONLY
    };
    AttributionMode attributionMode;

    // BY_FLAGS parameters
    uint32_t requiredFlags;         // Flags that must be set
    uint32_t forbiddenFlags;        // Flags that must NOT be set

    // BY_ADDRESS_RANGE parameters
    uint64_t vaStart;
    uint64_t vaEnd;

    // CUSTOM filter
    std::function<bool(const PageMetadata&)> customFilter;

    // Combination mode
    enum CombineMode {
        AND,    // All criteria must match
        OR      // Any criterion must match
    };
    CombineMode combineMode;

    // Constructor with defaults
    FilterCriteria()
        : type(NONE), includePid0(true),
          attributionMode(ALL_PAGES),
          requiredFlags(0), forbiddenFlags(0),
          vaStart(0), vaEnd(0xFFFFFFFFFFFFFFFFULL),
          combineMode(AND) {}

    // Convenience constructors
    static FilterCriteria ByPID(uint32_t pid) {
        FilterCriteria c;
        c.type = BY_PID;
        c.pids.push_back(pid);
        c.includePid0 = false;
        return c;
    }

    static FilterCriteria ByOwnership(PageMetadata::OwnershipType type) {
        FilterCriteria c;
        c.type = BY_OWNERSHIP;
        c.ownershipTypes.push_back(type);
        return c;
    }

    static FilterCriteria UnattributedOnly() {
        FilterCriteria c;
        c.type = BY_ATTRIBUTION;
        c.attributionMode = UNATTRIBUTED_ONLY;
        return c;
    }

    static FilterCriteria AttributedOnly() {
        FilterCriteria c;
        c.type = BY_ATTRIBUTION;
        c.attributionMode = ATTRIBUTED_ONLY;
        return c;
    }

    // Test if a page matches this criteria
    bool Matches(const PageMetadata& page) const;
};

/**
 * SortCriteria - Specifies how to order pages in a view
 */
struct SortCriteria {
    enum SortKey {
        PHYSICAL_ADDR,      // Sort by physical address
        VIRTUAL_ADDR,       // Sort by virtual address
        PID,                // Sort by process ID
        OWNERSHIP_TYPE,     // Sort by ownership type
        CUSTOM              // Custom comparator
    };

    SortKey primaryKey;
    SortKey secondaryKey;   // For tie-breaking

    bool ascending;         // true = ascending, false = descending

    // CUSTOM comparator
    std::function<bool(const PageMetadata&, const PageMetadata&)> customComparator;

    // Constructor with defaults
    SortCriteria()
        : primaryKey(PHYSICAL_ADDR), secondaryKey(PHYSICAL_ADDR),
          ascending(true) {}

    // Convenience constructors
    static SortCriteria ByPhysical(bool ascending = true) {
        SortCriteria c;
        c.primaryKey = PHYSICAL_ADDR;
        c.ascending = ascending;
        return c;
    }

    static SortCriteria ByVirtual(bool ascending = true) {
        SortCriteria c;
        c.primaryKey = VIRTUAL_ADDR;
        c.secondaryKey = PID;
        c.ascending = ascending;
        return c;
    }

    static SortCriteria ByPID(bool ascending = true) {
        SortCriteria c;
        c.primaryKey = PID;
        c.secondaryKey = VIRTUAL_ADDR;
        c.ascending = ascending;
        return c;
    }

    // Compare two pages according to this criteria
    bool Compare(const PageMetadata& a, const PageMetadata& b) const;
};

/**
 * MemoryView - A filtered and sorted view of pages from PageDatabase
 *
 * Represents a "crunched" address space based on arbitrary filter/sort criteria.
 * Replaces the per-PID crunching with a more general query system.
 */
class MemoryView {
public:
    MemoryView();
    ~MemoryView();

    // Build view from page database
    void BuildFromDatabase(const PageDatabase* database,
                           const FilterCriteria& filter,
                           const SortCriteria& sort);

    // Rebuild with new criteria (doesn't change database)
    void Rebuild(const FilterCriteria& filter, const SortCriteria& sort);

    // Get resulting memory regions (compatible with existing GuestMemoryRegion)
    // This converts the sorted page list into contiguous regions for rendering
    std::vector<GuestMemoryRegion> GetRegions() const;

    // Get individual pages (for detailed analysis)
    const std::vector<const PageMetadata*>& GetPages() const { return pages; }

    // Get total size of view (sum of all page sizes)
    uint64_t GetTotalSize() const { return pages.size() * 4096; }

    // Get view statistics
    struct ViewStats {
        size_t totalPages;
        size_t uniquePIDs;
        std::unordered_map<PageMetadata::OwnershipType, size_t> pagesByType;
    };
    ViewStats GetStats() const;

    // Get human-readable description of this view
    std::string GetDescription() const;

private:
    const PageDatabase* database;
    FilterCriteria currentFilter;
    SortCriteria currentSort;

    // Filtered and sorted page list (pointers into database)
    std::vector<const PageMetadata*> pages;

    // Helper: group consecutive pages into regions
    void CoalesceIntoRegions(std::vector<GuestMemoryRegion>& regions) const;
};

/**
 * ViewPresets - Common filter/sort combinations
 */
class ViewPresets {
public:
    // Single process view (current behavior)
    static std::pair<FilterCriteria, SortCriteria> SingleProcess(uint32_t pid) {
        return {
            FilterCriteria::ByPID(pid),
            SortCriteria::ByVirtual(true)
        };
    }

    // All stacks across all processes
    static std::pair<FilterCriteria, SortCriteria> AllStacks() {
        return {
            FilterCriteria::ByOwnership(PageMetadata::STACK),
            SortCriteria::ByPID(true)
        };
    }

    // All unattributed pages
    static std::pair<FilterCriteria, SortCriteria> UnattributedPages() {
        return {
            FilterCriteria::UnattributedOnly(),
            SortCriteria::ByPhysical(true)
        };
    }

    // All executable pages (code hunting)
    static std::pair<FilterCriteria, SortCriteria> ExecutablePages() {
        return {
            FilterCriteria::ByOwnership(PageMetadata::EXECUTABLE),
            SortCriteria::ByPID(true)
        };
    }

    // All writable pages (data hunting)
    static std::pair<FilterCriteria, SortCriteria> WritablePages() {
        FilterCriteria filter;
        filter.type = FilterCriteria::BY_FLAGS;
        filter.requiredFlags = 0x02;  // VM_WRITE
        return { filter, SortCriteria::ByPID(true) };
    }

    // Physical memory scan (no filtering, physical order)
    static std::pair<FilterCriteria, SortCriteria> PhysicalScan() {
        return {
            FilterCriteria(),  // No filter
            SortCriteria::ByPhysical(true)
        };
    }
};

} // namespace Haywire
