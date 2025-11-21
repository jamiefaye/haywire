#pragma once

#include "imgui.h"
#include "page_database.h"
#include "memory_view.h"
#include "memory_types.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>

namespace Haywire {

/**
 * PageDatabaseQuery - Interactive query interface for PageDatabase
 *
 * Provides UI for filtering and analyzing pages by:
 * - Process ID(s)
 * - Ownership type (executable, stack, heap, etc.)
 * - Memory flags (R/W/X)
 * - Attribution status
 * - Custom queries
 */
class PageDatabaseQuery {
public:
    PageDatabaseQuery();
    ~PageDatabaseQuery();

    // Set the page database to query
    void SetPageDatabase(std::shared_ptr<PageDatabase> db);

    // Set callback for when user wants to view results in visualizer
    using ViewResultsCallback = std::function<void(const std::vector<SectionEntry>&,
                                                     const std::unordered_map<uint64_t, uint64_t>&,
                                                     const std::string&)>;
    void SetViewResultsCallback(ViewResultsCallback callback) { viewResultsCallback = callback; }

    // Show/hide the query window
    void Show() { visible = true; }
    void Hide() { visible = false; }
    void ToggleVisible() { visible = !visible; }
    bool IsVisible() const { return visible; }

    // Render the query window (call in main loop)
    void Render();

private:
    std::shared_ptr<PageDatabase> pageDB;
    bool visible;
    ViewResultsCallback viewResultsCallback;

    // Query parameters
    struct QueryParams {
        // Filter type selection
        int filterType;  // 0=By PID, 1=By Name, 2=By Ownership, 3=By Flags, 4=Unattributed, 5=All

        // By PID filter
        int selectedPID;
        char pidInput[16];

        // By Name filter (with wildcard support)
        char namePattern[64];
        std::vector<uint32_t> matchingPIDs;  // PIDs that match the name pattern

        // By Ownership filter
        bool ownershipFilters[11];  // One for each PageMetadata::OwnershipType

        // By Flags filter
        bool requireRead;
        bool requireWrite;
        bool requireExec;
        bool requireShared;
        bool forbidWrite;
        bool forbidExec;

        // Attribution filter
        bool showAttributed;
        bool showUnattributed;

        // Display options
        bool deduplicateSharedPages;  // If true, show each page once even if owned by multiple PIDs
        int sortMode;  // 0=By Physical Addr, 1=By PID, 2=By Virtual Addr

        QueryParams() {
            filterType = 5;  // All pages by default
            selectedPID = 0;
            pidInput[0] = '\0';
            namePattern[0] = '\0';

            // Enable all ownership types by default
            for (int i = 0; i < 11; i++) {
                ownershipFilters[i] = true;
            }

            requireRead = false;
            requireWrite = false;
            requireExec = false;
            requireShared = false;
            forbidWrite = false;
            forbidExec = false;

            showAttributed = true;
            showUnattributed = true;

            deduplicateSharedPages = false;  // Show all by default
            sortMode = 0;  // Physical address by default
        }
    };

    QueryParams params;

    // Query results
    struct QueryResults {
        std::vector<PageMetadata> pages;  // Store copies, not pointers (thread-safe)
        size_t totalPages;
        size_t totalBytes;
        std::unordered_map<PageMetadata::OwnershipType, size_t> pagesByType;
        std::unordered_map<uint32_t, size_t> pagesByPID;
        bool hasResults;

        QueryResults() : totalPages(0), totalBytes(0), hasResults(false) {}
    };

    QueryResults results;

    // Execute current query
    void ExecuteQuery();

    // Build FilterCriteria from current params
    FilterCriteria BuildFilterCriteria() const;

    // Render different sections
    void RenderFilterControls();
    void RenderResults();
    void RenderStatistics();

    // Helper to get ownership type name
    const char* GetOwnershipTypeName(PageMetadata::OwnershipType type) const;

    // Wildcard matching (supports * and ?)
    bool MatchesWildcard(const char* str, const char* pattern) const;

    // Find all PIDs matching the name pattern
    void UpdateMatchingPIDs();
};

} // namespace Haywire
