#include "page_database_query.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace Haywire {

PageDatabaseQuery::PageDatabaseQuery()
    : pageDB(nullptr), visible(false) {
}

PageDatabaseQuery::~PageDatabaseQuery() {
}

void PageDatabaseQuery::SetPageDatabase(std::shared_ptr<PageDatabase> db) {
    pageDB = db;
}

void PageDatabaseQuery::Render() {
    if (!visible) return;
    if (!pageDB) return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Page Database Query", &visible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // Check if database is ready
    bool isComplete = pageDB->IsFullScanComplete();
    size_t scanned = pageDB->GetScannedProcessCount();
    size_t total = pageDB->GetTotalProcessCount();

    if (!isComplete) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                          "Database still scanning: %zu/%zu processes", scanned, total);
        ImGui::Text("Queries may return incomplete results.");
        ImGui::Separator();
    }

    // Filter controls
    RenderFilterControls();

    ImGui::Separator();

    // Execute query button
    if (ImGui::Button("Execute Query", ImVec2(120, 0))) {
        ExecuteQuery();
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear Results", ImVec2(120, 0))) {
        results.hasResults = false;
        results.pages.clear();
    }

    ImGui::SameLine();
    if (results.hasResults && ImGui::Button("View in Visualizer", ImVec2(150, 0))) {
        // Convert results to sections and PTEs
        std::vector<SectionEntry> sections;
        std::unordered_map<uint64_t, uint64_t> ptes;

        // Use MemoryView to get regions (already coalesced)
        MemoryView view;
        view.BuildFromDatabase(pageDB.get(), BuildFilterCriteria(), SortCriteria::ByVirtual(true));

        auto regions = view.GetRegions();

        // Convert regions to SectionEntry
        for (const auto& region : regions) {
            SectionEntry section;
            section.va_start = region.start;
            section.va_end = region.end;
            section.ownership_type = static_cast<uint32_t>(region.ownershipType);

            // Parse permissions string
            section.perms = 0;
            if (region.permissions.find('r') != std::string::npos) section.perms |= 0x01;
            if (region.permissions.find('w') != std::string::npos) section.perms |= 0x02;
            if (region.permissions.find('x') != std::string::npos) section.perms |= 0x04;
            if (region.permissions.find('s') != std::string::npos) section.perms |= 0x08;

            strncpy(section.path, region.name.c_str(), sizeof(section.path) - 1);
            section.path[sizeof(section.path) - 1] = '\0';

            sections.push_back(section);
        }

        // Build PTEs from all pages in results
        for (const auto* page : results.pages) {
            if (page->virtualAddr != 0) {
                ptes[page->virtualAddr] = page->physicalAddr;
            }
        }

        // Build description
        std::ostringstream desc;
        desc << "Query: ";
        switch (params.filterType) {
            case 0: desc << "PID " << params.selectedPID; break;
            case 1: desc << "Name '" << params.namePattern << "' (" << params.matchingPIDs.size() << " processes)"; break;
            case 2: desc << "By Ownership Type"; break;
            case 3: desc << "By Flags"; break;
            case 4: desc << "Unattributed Pages"; break;
            case 5: desc << "All Pages"; break;
        }
        desc << " (" << results.totalPages << " pages)";

        // Call callback if set
        if (viewResultsCallback) {
            viewResultsCallback(sections, ptes, desc.str());
        }
    }

    ImGui::Separator();

    // Results
    if (results.hasResults) {
        RenderResults();
    }

    ImGui::End();
}

void PageDatabaseQuery::RenderFilterControls() {
    ImGui::Text("Filter Type:");
    ImGui::RadioButton("By PID", &params.filterType, 0); ImGui::SameLine();
    ImGui::RadioButton("By Name", &params.filterType, 1); ImGui::SameLine();
    ImGui::RadioButton("By Ownership", &params.filterType, 2);
    ImGui::RadioButton("By Flags", &params.filterType, 3); ImGui::SameLine();
    ImGui::RadioButton("Unattributed", &params.filterType, 4); ImGui::SameLine();
    ImGui::RadioButton("All Pages", &params.filterType, 5);

    ImGui::Spacing();

    // Filter-specific controls
    if (params.filterType == 0) {  // By PID
        ImGui::Text("PID:");
        ImGui::SameLine();
        if (ImGui::InputText("##pid", params.pidInput, sizeof(params.pidInput))) {
            // Parse automatically as user types
            params.selectedPID = atoi(params.pidInput);
        }
        if (params.selectedPID > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "→ Will query PID %d", params.selectedPID);
        } else if (strlen(params.pidInput) > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "→ Invalid PID");
        }
    }
    else if (params.filterType == 1) {  // By Name (with wildcards)
        ImGui::Text("Process Name Pattern (supports * and ?):");
        if (ImGui::InputText("##name", params.namePattern, sizeof(params.namePattern))) {
            // Update matching PIDs as user types
            UpdateMatchingPIDs();
        }

        if (strlen(params.namePattern) > 0) {
            if (params.matchingPIDs.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                                 "No processes match '%s'", params.namePattern);
            } else {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                 "Matched %zu processes:", params.matchingPIDs.size());
                ImGui::Indent();
                // Show first 10 matching processes
                int shown = 0;
                for (uint32_t pid : params.matchingPIDs) {
                    if (shown >= 10) {
                        ImGui::Text("... and %zu more", params.matchingPIDs.size() - 10);
                        break;
                    }
                    ImGui::Text("PID %u", pid);
                    shown++;
                }
                ImGui::Unindent();
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                             "Examples: vlc, *firefox*, chrome*, *");
        }
    }
    else if (params.filterType == 2) {  // By Ownership Type
        ImGui::Text("Select ownership types to include:");
        ImGui::Columns(2, "ownership_cols", false);

        const char* typeNames[] = {
            "Unattributed", "Anonymous", "File-backed", "Shared Library",
            "Executable", "Stack", "Heap", "vDSO", "vvar",
            "Page Cache", "Kernel Data"
        };

        for (int i = 0; i < 11; i++) {
            ImGui::Checkbox(typeNames[i], &params.ownershipFilters[i]);
            if (i == 5) ImGui::NextColumn();  // Split at halfway point
        }
        ImGui::Columns(1);
    }
    else if (params.filterType == 3) {  // By Flags
        ImGui::Text("Required flags (all must be set):");
        ImGui::Checkbox("Readable", &params.requireRead);
        ImGui::SameLine();
        ImGui::Checkbox("Writable", &params.requireWrite);
        ImGui::SameLine();
        ImGui::Checkbox("Executable", &params.requireExec);
        ImGui::SameLine();
        ImGui::Checkbox("Shared", &params.requireShared);

        ImGui::Text("Forbidden flags (none can be set):");
        ImGui::Checkbox("Forbid Write", &params.forbidWrite);
        ImGui::SameLine();
        ImGui::Checkbox("Forbid Execute", &params.forbidExec);
    }

    // Display options (shown for all filter types)
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Display Options:");
    ImGui::Checkbox("Deduplicate shared pages (show each page once)", &params.deduplicateSharedPages);
    ImGui::Text("Sort by:");
    ImGui::RadioButton("Physical Address", &params.sortMode, 0); ImGui::SameLine();
    ImGui::RadioButton("PID", &params.sortMode, 1); ImGui::SameLine();
    ImGui::RadioButton("Virtual Address", &params.sortMode, 2);
}

void PageDatabaseQuery::RenderResults() {
    ImGui::Text("Query Results: %zu pages (%zu MB)",
                results.totalPages,
                results.totalBytes / (1024 * 1024));

    if (ImGui::BeginTabBar("ResultTabs")) {
        if (ImGui::BeginTabItem("Statistics")) {
            RenderStatistics();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Page List")) {
            // Show first 100 pages
            ImGui::BeginChild("PageList", ImVec2(0, 400), true);

            ImGui::Columns(6, "page_columns");
            ImGui::Text("Physical Addr"); ImGui::NextColumn();
            ImGui::Text("Virtual Addr"); ImGui::NextColumn();
            ImGui::Text("PID"); ImGui::NextColumn();
            ImGui::Text("Type"); ImGui::NextColumn();
            ImGui::Text("Flags"); ImGui::NextColumn();
            ImGui::Text("File"); ImGui::NextColumn();
            ImGui::Separator();

            size_t showCount = std::min(results.pages.size(), size_t(100));
            for (size_t i = 0; i < showCount; i++) {
                const auto* page = results.pages[i];

                ImGui::Text("0x%lx", page->physicalAddr); ImGui::NextColumn();
                ImGui::Text("0x%lx", page->virtualAddr); ImGui::NextColumn();

                // Display all PIDs comma-separated
                std::string pidList;
                for (size_t i = 0; i < page->pids.size(); i++) {
                    if (i > 0) pidList += ",";
                    pidList += std::to_string(page->pids[i]);
                }
                ImGui::Text("%s", pidList.empty() ? "0" : pidList.c_str()); ImGui::NextColumn();

                ImGui::Text("%s", GetOwnershipTypeName(page->ownershipType)); ImGui::NextColumn();

                // Flags
                char flags[5] = "----";
                if (page->isReadable()) flags[0] = 'r';
                if (page->isWritable()) flags[1] = 'w';
                if (page->isExecutable()) flags[2] = 'x';
                if (page->isShared()) flags[3] = 's';
                ImGui::Text("%s", flags); ImGui::NextColumn();

                ImGui::Text("%s", page->filename.c_str()); ImGui::NextColumn();
            }

            if (results.pages.size() > 100) {
                ImGui::Separator();
                ImGui::Text("... and %zu more pages", results.pages.size() - 100);
            }

            ImGui::Columns(1);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void PageDatabaseQuery::RenderStatistics() {
    ImGui::Text("Total Pages: %zu", results.totalPages);
    ImGui::Text("Total Size: %zu MB", results.totalBytes / (1024 * 1024));

    ImGui::Spacing();
    ImGui::Text("Pages by Ownership Type:");
    ImGui::Separator();

    for (const auto& [type, count] : results.pagesByType) {
        float percent = (float)count / results.totalPages * 100.0f;
        ImGui::Text("  %-20s: %6zu pages (%5.1f%%)",
                   GetOwnershipTypeName(type), count, percent);
    }

    if (!results.pagesByPID.empty()) {
        ImGui::Spacing();
        ImGui::Text("Pages by Process (top 20):");
        ImGui::Separator();

        // Sort by page count
        std::vector<std::pair<uint32_t, size_t>> pidCounts(
            results.pagesByPID.begin(), results.pagesByPID.end());
        std::sort(pidCounts.begin(), pidCounts.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });

        size_t showCount = std::min(pidCounts.size(), size_t(20));
        for (size_t i = 0; i < showCount; i++) {
            float percent = (float)pidCounts[i].second / results.totalPages * 100.0f;
            ImGui::Text("  PID %5u: %6zu pages (%5.1f%%)",
                       pidCounts[i].first, pidCounts[i].second, percent);
        }

        if (pidCounts.size() > 20) {
            ImGui::Text("  ... and %zu more processes", pidCounts.size() - 20);
        }
    }
}

void PageDatabaseQuery::ExecuteQuery() {
    if (!pageDB) return;

    results = QueryResults();  // Reset

    FilterCriteria filter = BuildFilterCriteria();
    SortCriteria sort = SortCriteria::ByPhysical(true);

    // Debug: Log filter details
    if (filter.type == FilterCriteria::BY_PID && !filter.pids.empty()) {
        std::cout << "[Query] Filtering by PID " << filter.pids[0] << "\n";
        std::cout << "[Query] Database has scanned " << pageDB->IsPIDScanned(filter.pids[0])
                  << " for this PID\n";

        // Count total pages for this PID in database
        auto allPages = pageDB->GetAllPages();
        size_t pidPageCount = 0;
        for (const auto& page : allPages) {
            if (page.hasProcess(filter.pids[0])) {
                pidPageCount++;
            }
        }
        std::cout << "[Query] Database contains " << pidPageCount << " pages for PID "
                  << filter.pids[0] << "\n";
    }

    // Create a memory view with our filter
    MemoryView view;
    view.BuildFromDatabase(pageDB.get(), filter, sort);

    results.pages = view.GetPages();
    results.totalPages = results.pages.size();
    results.totalBytes = results.totalPages * 4096;

    std::cout << "[Query] Filter returned " << results.totalPages << " pages\n";

    // Calculate statistics
    for (const auto* page : results.pages) {
        results.pagesByType[page->ownershipType]++;
        // Count page for each owning PID
        for (uint32_t pid : page->pids) {
            results.pagesByPID[pid]++;
        }
    }

    results.hasResults = true;
}

FilterCriteria PageDatabaseQuery::BuildFilterCriteria() const {
    FilterCriteria filter;

    switch (params.filterType) {
        case 0:  // By PID
            filter.type = FilterCriteria::BY_PID;
            filter.pids.push_back(params.selectedPID);
            filter.includePid0 = false;
            break;

        case 1:  // By Name (with wildcards)
            filter.type = FilterCriteria::BY_PID;
            filter.pids = params.matchingPIDs;  // Use all matching PIDs
            filter.includePid0 = false;
            break;

        case 2:  // By Ownership Type
            filter.type = FilterCriteria::BY_OWNERSHIP;
            for (int i = 0; i < 11; i++) {
                if (params.ownershipFilters[i]) {
                    filter.ownershipTypes.push_back(
                        static_cast<PageMetadata::OwnershipType>(i));
                }
            }
            break;

        case 3:  // By Flags
            filter.type = FilterCriteria::BY_FLAGS;
            filter.requiredFlags = 0;
            filter.forbiddenFlags = 0;

            if (params.requireRead) filter.requiredFlags |= 0x01;
            if (params.requireWrite) filter.requiredFlags |= 0x02;
            if (params.requireExec) filter.requiredFlags |= 0x04;
            if (params.requireShared) filter.requiredFlags |= 0x08;

            if (params.forbidWrite) filter.forbiddenFlags |= 0x02;
            if (params.forbidExec) filter.forbiddenFlags |= 0x04;
            break;

        case 4:  // Unattributed only
            filter.type = FilterCriteria::BY_ATTRIBUTION;
            filter.attributionMode = FilterCriteria::UNATTRIBUTED_ONLY;
            break;

        case 5:  // All pages
        default:
            filter.type = FilterCriteria::NONE;
            break;
    }

    return filter;
}

const char* PageDatabaseQuery::GetOwnershipTypeName(PageMetadata::OwnershipType type) const {
    switch (type) {
        case PageMetadata::UNATTRIBUTED: return "Unattributed";
        case PageMetadata::ANONYMOUS: return "Anonymous";
        case PageMetadata::FILE_BACKED: return "File-backed";
        case PageMetadata::SHARED_LIB: return "Shared Library";
        case PageMetadata::EXECUTABLE: return "Executable";
        case PageMetadata::STACK: return "Stack";
        case PageMetadata::HEAP: return "Heap";
        case PageMetadata::VDSO: return "vDSO";
        case PageMetadata::VVAR: return "vvar";
        case PageMetadata::PAGE_CACHE: return "Page Cache";
        case PageMetadata::KERNEL_DATA: return "Kernel Data";
        default: return "Unknown";
    }
}

// Wildcard pattern matching (supports * and ?)
bool PageDatabaseQuery::MatchesWildcard(const char* str, const char* pattern) const {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;  // * at end matches everything

            // Try to match rest of pattern with remaining string
            while (*str) {
                if (MatchesWildcard(str, pattern)) return true;
                str++;
            }
            return false;
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return false;
        }
    }
    return !*str;
}

// Find all PIDs matching the name pattern
void PageDatabaseQuery::UpdateMatchingPIDs() {
    params.matchingPIDs.clear();

    if (!pageDB || strlen(params.namePattern) == 0) {
        return;
    }

    // Get all process names from database
    auto processNames = pageDB->GetAllProcessNames();

    // For each process, check if the name matches the pattern
    for (const auto& [pid, name] : processNames) {
        if (MatchesWildcard(name.c_str(), params.namePattern)) {
            params.matchingPIDs.push_back(pid);
        }
    }

    // Sort PIDs for consistent display
    std::sort(params.matchingPIDs.begin(), params.matchingPIDs.end());
}

} // namespace Haywire
