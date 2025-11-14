#include "memory_view.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace Haywire {

// FilterCriteria::Matches implementation
bool FilterCriteria::Matches(const PageMetadata& page) const {
    switch (type) {
        case NONE:
            return true;

        case BY_PID: {
            // Check if page has any matching PID
            if (page.pids.empty()) {
                return includePid0;  // Unattributed page
            }
            if (pids.empty()) {
                return true;  // Empty filter list = all PIDs
            }
            // Check if any of the page's PIDs match any of the filter PIDs
            for (uint32_t pagePid : page.pids) {
                if (std::find(pids.begin(), pids.end(), pagePid) != pids.end()) {
                    return true;  // Found a match
                }
            }
            return false;  // No match
        }

        case BY_OWNERSHIP: {
            if (ownershipTypes.empty()) {
                return true;  // Empty list = all types
            }
            return std::find(ownershipTypes.begin(), ownershipTypes.end(),
                           page.ownershipType) != ownershipTypes.end();
        }

        case BY_ATTRIBUTION: {
            switch (attributionMode) {
                case ALL_PAGES:
                    return true;
                case ATTRIBUTED_ONLY:
                    return page.isAttributed();
                case UNATTRIBUTED_ONLY:
                    return !page.isAttributed();
            }
            return false;
        }

        case BY_FLAGS: {
            // Check required flags
            if (requiredFlags && (page.flags & requiredFlags) != requiredFlags) {
                return false;
            }
            // Check forbidden flags
            if (forbiddenFlags && (page.flags & forbiddenFlags) != 0) {
                return false;
            }
            return true;
        }

        case BY_ADDRESS_RANGE: {
            // Only filter attributed pages by VA range
            if (!page.isAttributed()) {
                return false;
            }
            return page.virtualAddr >= vaStart && page.virtualAddr < vaEnd;
        }

        case CUSTOM: {
            if (customFilter) {
                return customFilter(page);
            }
            return true;
        }
    }

    return false;
}

// SortCriteria::Compare implementation
bool SortCriteria::Compare(const PageMetadata& a, const PageMetadata& b) const {
    auto compareByKey = [](SortKey key, const PageMetadata& x, const PageMetadata& y) -> int {
        switch (key) {
            case PHYSICAL_ADDR:
                if (x.physicalAddr < y.physicalAddr) return -1;
                if (x.physicalAddr > y.physicalAddr) return 1;
                return 0;

            case VIRTUAL_ADDR:
                if (x.virtualAddr < y.virtualAddr) return -1;
                if (x.virtualAddr > y.virtualAddr) return 1;
                return 0;

            case PID: {
                // Use first PID for sorting (most pages have one PID anyway)
                uint32_t xPid = x.pids.empty() ? 0 : x.pids[0];
                uint32_t yPid = y.pids.empty() ? 0 : y.pids[0];
                if (xPid < yPid) return -1;
                if (xPid > yPid) return 1;
                return 0;
            }

            case OWNERSHIP_TYPE:
                if (x.ownershipType < y.ownershipType) return -1;
                if (x.ownershipType > y.ownershipType) return 1;
                return 0;

            case CUSTOM:
                return 0;  // Handled separately
        }
        return 0;
    };

    // Handle custom comparator
    if (primaryKey == CUSTOM && customComparator) {
        return customComparator(a, b);
    }

    // Primary key comparison
    int cmp = compareByKey(primaryKey, a, b);
    if (cmp != 0) {
        return ascending ? (cmp < 0) : (cmp > 0);
    }

    // Secondary key for tie-breaking
    if (secondaryKey != primaryKey) {
        cmp = compareByKey(secondaryKey, a, b);
        if (cmp != 0) {
            return ascending ? (cmp < 0) : (cmp > 0);
        }
    }

    // Final tie-breaker: physical address (stable sort)
    return ascending ? (a.physicalAddr < b.physicalAddr) : (a.physicalAddr > b.physicalAddr);
}

// MemoryView implementation
MemoryView::MemoryView()
    : database(nullptr) {
}

MemoryView::~MemoryView() {
}

void MemoryView::BuildFromDatabase(const PageDatabase* db,
                                   const FilterCriteria& filter,
                                   const SortCriteria& sort) {
    database = db;
    currentFilter = filter;
    currentSort = sort;

    pages.clear();
    pageStorage.clear();

    if (!database) return;

    // Get all pages from database
    auto allPages = database->GetAllPages();

    // Copy filtered pages into our storage
    size_t matchCount = 0;
    size_t pid3101Count = 0;
    for (const auto& page : allPages) {
        if (page.hasProcess(3101)) pid3101Count++;
        if (filter.Matches(page)) {
            pageStorage.push_back(page);
            matchCount++;
        }
    }

    if (filter.type == FilterCriteria::BY_PID && !filter.pids.empty()) {
        std::cout << "[MemoryView] Filter debug: Looking for PID " << filter.pids[0]
                  << ", found " << pid3101Count << " pages with PID 3101 in database"
                  << ", matched " << matchCount << " pages\n";
    }

    // Build pointer list from storage
    for (const auto& page : pageStorage) {
        pages.push_back(&page);
    }

    std::cout << "[MemoryView] Built view: " << allPages.size() << " total pages, "
              << pageStorage.size() << " after filter, "
              << pages.size() << " pointers created\n";

    // Sort pages
    std::sort(pages.begin(), pages.end(),
              [&sort](const PageMetadata* a, const PageMetadata* b) {
                  return sort.Compare(*a, *b);
              });
}

void MemoryView::Rebuild(const FilterCriteria& filter, const SortCriteria& sort) {
    BuildFromDatabase(database, filter, sort);
}

std::vector<GuestMemoryRegion> MemoryView::GetRegions() const {
    std::vector<GuestMemoryRegion> regions;
    CoalesceIntoRegions(regions);
    return regions;
}

void MemoryView::CoalesceIntoRegions(std::vector<GuestMemoryRegion>& regions) const {
    if (pages.empty()) return;

    GuestMemoryRegion currentRegion;
    currentRegion.start = pages[0]->virtualAddr;
    currentRegion.end = pages[0]->virtualAddr + 4096;
    currentRegion.name = pages[0]->filename;
    currentRegion.ownershipType = static_cast<GuestMemoryRegion::OwnershipType>(pages[0]->ownershipType);

    // Build permissions string
    std::string perms;
    if (pages[0]->isReadable()) perms += 'r'; else perms += '-';
    if (pages[0]->isWritable()) perms += 'w'; else perms += '-';
    if (pages[0]->isExecutable()) perms += 'x'; else perms += '-';
    if (pages[0]->isShared()) perms += 's'; else perms += 'p';
    currentRegion.permissions = perms;

    for (size_t i = 1; i < pages.size(); i++) {
        const PageMetadata* page = pages[i];

        // Check if this page can be merged with current region
        bool canMerge = false;

        // For physical address sorting, merge consecutive physical pages
        if (currentSort.primaryKey == SortCriteria::PHYSICAL_ADDR) {
            canMerge = (page->physicalAddr == currentRegion.end);
        }
        // For virtual address sorting, merge consecutive VA pages in same process
        else if (currentSort.primaryKey == SortCriteria::VIRTUAL_ADDR) {
            // Pages are in same process if they share any PID
            bool samePIDs = page->pids == pages[i-1]->pids;
            canMerge = (page->virtualAddr == currentRegion.end &&
                       samePIDs &&
                       page->filename == currentRegion.name &&
                       page->flags == pages[i-1]->flags);
        }
        // For PID sorting, merge pages in same process with same properties
        else if (currentSort.primaryKey == SortCriteria::PID) {
            bool samePIDs = page->pids == pages[i-1]->pids;
            canMerge = (samePIDs &&
                       page->ownershipType == currentRegion.ownershipType &&
                       page->filename == currentRegion.name);
        }
        // For ownership type sorting, merge pages of same type
        else if (currentSort.primaryKey == SortCriteria::OWNERSHIP_TYPE) {
            canMerge = (page->ownershipType == currentRegion.ownershipType);
        }

        if (canMerge) {
            // Extend current region
            currentRegion.end = page->virtualAddr + 4096;
        } else {
            // Save current region and start new one
            regions.push_back(currentRegion);

            currentRegion.start = page->virtualAddr;
            currentRegion.end = page->virtualAddr + 4096;
            currentRegion.name = page->filename;
            currentRegion.ownershipType = static_cast<GuestMemoryRegion::OwnershipType>(page->ownershipType);

            perms.clear();
            if (page->isReadable()) perms += 'r'; else perms += '-';
            if (page->isWritable()) perms += 'w'; else perms += '-';
            if (page->isExecutable()) perms += 'x'; else perms += '-';
            if (page->isShared()) perms += 's'; else perms += 'p';
            currentRegion.permissions = perms;
        }
    }

    // Save final region
    regions.push_back(currentRegion);
}

MemoryView::ViewStats MemoryView::GetStats() const {
    ViewStats stats;
    stats.totalPages = pages.size();

    std::unordered_set<uint32_t> pids;

    for (const auto* page : pages) {
        // Add all PIDs from this page
        for (uint32_t pid : page->pids) {
            pids.insert(pid);
        }
        stats.pagesByType[page->ownershipType]++;
    }

    stats.uniquePIDs = pids.size();
    return stats;
}

std::string MemoryView::GetDescription() const {
    std::ostringstream oss;

    // Describe filter
    switch (currentFilter.type) {
        case FilterCriteria::NONE:
            oss << "All pages";
            break;
        case FilterCriteria::BY_PID:
            if (currentFilter.pids.size() == 1) {
                oss << "PID " << currentFilter.pids[0];
            } else if (currentFilter.pids.empty()) {
                oss << "All PIDs";
            } else {
                oss << currentFilter.pids.size() << " PIDs";
            }
            break;
        case FilterCriteria::BY_OWNERSHIP:
            if (currentFilter.ownershipTypes.size() == 1) {
                PageMetadata temp;
                temp.ownershipType = currentFilter.ownershipTypes[0];
                oss << temp.getTypeName();
            } else {
                oss << "Multiple types";
            }
            break;
        case FilterCriteria::BY_ATTRIBUTION:
            switch (currentFilter.attributionMode) {
                case FilterCriteria::ALL_PAGES:
                    oss << "All pages";
                    break;
                case FilterCriteria::ATTRIBUTED_ONLY:
                    oss << "Attributed pages";
                    break;
                case FilterCriteria::UNATTRIBUTED_ONLY:
                    oss << "Unattributed pages";
                    break;
            }
            break;
        default:
            oss << "Custom filter";
    }

    // Describe sort
    oss << " (sorted by ";
    switch (currentSort.primaryKey) {
        case SortCriteria::PHYSICAL_ADDR:
            oss << "physical address";
            break;
        case SortCriteria::VIRTUAL_ADDR:
            oss << "virtual address";
            break;
        case SortCriteria::PID:
            oss << "PID";
            break;
        case SortCriteria::OWNERSHIP_TYPE:
            oss << "type";
            break;
        default:
            oss << "custom";
    }
    oss << ")";

    return oss.str();
}

} // namespace Haywire
