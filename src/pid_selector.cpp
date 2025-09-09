#include "pid_selector.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

namespace Haywire {

PIDSelector::PIDSelector() 
    : isVisible(false), selectedPID(0), selectedCamera(1),
      sortColumn(SORT_PID), sortAscending(true),
      showKernelThreads(false), showOnlyWithDetails(false) {
    std::memset(filterText, 0, sizeof(filterText));
}

PIDSelector::~PIDSelector() {
}

void PIDSelector::RefreshPIDList() {
    if (!beaconReader) return;
    
    processes.clear();
    
    // Get raw PID list
    pidList.clear();
    if (!beaconReader->GetPIDList(pidList)) {
        std::cerr << "Failed to get PID list from beacon\n";
        return;
    }
    
    // Get process details from round-robin
    auto processInfo = beaconReader->GetAllProcessInfo();
    
    std::cout << "PIDSelector: Got " << processInfo.size() << " processes with info from beacon\n";
    
    // Build display entries
    for (uint32_t pid : pidList) {
        ProcessDisplayEntry entry;
        entry.pid = pid;
        
        // Check if we have details from round-robin
        auto it = processInfo.find(pid);
        if (it != processInfo.end()) {
            const auto& info = it->second;
            entry.ppid = info.ppid;
            entry.name = info.name;
            entry.exe = info.exe_path;
            entry.state = info.state;
            entry.vsizeMB = info.vsize / (1024 * 1024);
            entry.rssMB = (info.rss * 4096) / (1024 * 1024);  // rss is in pages
            entry.threads = info.num_threads;
            entry.hasDetails = true;
        } else {
            // No details available yet
            entry.ppid = 0;
            entry.name = "PID " + std::to_string(pid);
            entry.exe = "";
            entry.state = '?';
            entry.vsizeMB = 0;
            entry.rssMB = 0;
            entry.threads = 0;
            entry.hasDetails = false;
        }
        
        processes.push_back(entry);
    }
    
    std::cout << "Refreshed PID list: " << processes.size() << " processes, "
              << processInfo.size() << " with details\n";
    
    SortProcessList();
}

void PIDSelector::Draw() {
    if (!isVisible) return;
    
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Process Selector", &isVisible)) {
        ImGui::End();
        return;
    }
    
    // Top controls
    ImGui::Text("Select a process to monitor:");
    ImGui::SameLine();
    ImGui::Text("(%zu processes)", processes.size());
    
    ImGui::Separator();
    
    // Filter and options
    ImGui::PushItemWidth(200);
    if (ImGui::InputText("Filter", filterText, sizeof(filterText))) {
        // Filter changed, could trigger re-sort
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Checkbox("Show kernel threads", &showKernelThreads);
    
    ImGui::SameLine();
    ImGui::Checkbox("Only with details", &showOnlyWithDetails);
    
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        RefreshPIDList();
    }
    
    // Camera selection
    ImGui::Text("Target Camera:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Camera 1", selectedCamera == 1)) selectedCamera = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Camera 2", selectedCamera == 2)) selectedCamera = 2;
    
    // Current focus
    if (beaconReader) {
        uint32_t cam1Focus = beaconReader->GetCameraFocus(1);
        uint32_t cam2Focus = beaconReader->GetCameraFocus(2);
        ImGui::SameLine();
        ImGui::Text("   Current: Cam1=%u, Cam2=%u", cam1Focus, cam2Focus);
    }
    
    ImGui::Separator();
    
    // Process table
    if (ImGui::BeginTable("ProcessTable", 6,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_BordersV)) {
        
        // Headers
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("VSZ (MB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("RSS (MB)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        
        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty) {
                if (sortSpecs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    sortColumn = static_cast<SortColumn>(spec.ColumnIndex);
                    sortAscending = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    SortProcessList();
                }
                sortSpecs->SpecsDirty = false;
            }
        }
        
        // Draw rows
        ImGuiListClipper clipper;
        
        // Count visible items
        int visibleCount = 0;
        for (const auto& proc : processes) {
            if (PassesFilter(proc)) visibleCount++;
        }
        
        clipper.Begin(visibleCount);
        
        int visibleIndex = 0;
        while (clipper.Step()) {
            int currentVisible = 0;
            
            for (const auto& proc : processes) {
                if (!PassesFilter(proc)) continue;
                
                if (currentVisible >= clipper.DisplayStart && currentVisible < clipper.DisplayEnd) {
                    ImGui::TableNextRow();
                    DrawProcessRow(proc, proc.pid == selectedPID);
                }
                
                currentVisible++;
                if (currentVisible >= clipper.DisplayEnd) break;
            }
        }
        
        ImGui::EndTable();
    }
    
    // Bottom section
    ImGui::Separator();
    
    // Info text
    ImGui::TextDisabled("Click any row to select and focus camera on that process");
    
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 80);
    if (ImGui::Button("Close")) {
        Hide();
    }
    
    ImGui::End();
}

void PIDSelector::SortProcessList() {
    std::sort(processes.begin(), processes.end(),
        [this](const ProcessDisplayEntry& a, const ProcessDisplayEntry& b) {
            int cmp = 0;
            
            switch (sortColumn) {
                case SORT_PID:
                    cmp = (a.pid < b.pid) ? -1 : (a.pid > b.pid) ? 1 : 0;
                    break;
                case SORT_NAME:
                    cmp = a.name.compare(b.name);
                    break;
                case SORT_STATE:
                    cmp = (a.state < b.state) ? -1 : (a.state > b.state) ? 1 : 0;
                    break;
                case SORT_VSIZE:
                    cmp = (a.vsizeMB < b.vsizeMB) ? -1 : (a.vsizeMB > b.vsizeMB) ? 1 : 0;
                    break;
                case SORT_RSS:
                    cmp = (a.rssMB < b.rssMB) ? -1 : (a.rssMB > b.rssMB) ? 1 : 0;
                    break;
                case SORT_THREADS:
                    cmp = (a.threads < b.threads) ? -1 : (a.threads > b.threads) ? 1 : 0;
                    break;
            }
            
            return sortAscending ? (cmp < 0) : (cmp > 0);
        });
}

bool PIDSelector::PassesFilter(const ProcessDisplayEntry& entry) const {
    // Check kernel thread filter
    if (!showKernelThreads && entry.ppid == 2 && entry.hasDetails) {
        return false;  // Kernel thread (parent is kthreadd)
    }
    
    // Check details filter
    if (showOnlyWithDetails && !entry.hasDetails) {
        return false;
    }
    
    // Check text filter
    if (filterText[0] != '\0') {
        std::string filter(filterText);
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
        
        // Check PID
        if (std::to_string(entry.pid).find(filter) != std::string::npos) {
            return true;
        }
        
        // Check name
        std::string nameLower = entry.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        if (nameLower.find(filter) != std::string::npos) {
            return true;
        }
        
        // Check exe path
        std::string exeLower = entry.exe;
        std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::tolower);
        if (exeLower.find(filter) != std::string::npos) {
            return true;
        }
        
        return false;
    }
    
    return true;
}

void PIDSelector::DrawProcessRow(const ProcessDisplayEntry& entry, bool isSelected) {
    // PID column
    ImGui::TableSetColumnIndex(0);
    
    if (ImGui::Selectable(std::to_string(entry.pid).c_str(), isSelected,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
        selectedPID = entry.pid;
        
        // Single-click to select immediately
        HandleSelection(entry.pid);
    }
    
    // Name column
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%s", entry.name.c_str());
    if (ImGui::IsItemHovered() && !entry.exe.empty()) {
        ImGui::SetTooltip("%s", entry.exe.c_str());
    }
    
    // State column
    ImGui::TableSetColumnIndex(2);
    if (entry.hasDetails) {
        ImGui::PushStyleColor(ImGuiCol_Text, entry.GetStateColor());
        ImGui::Text("%s", entry.GetStateString().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Unknown");
    }
    
    // VSZ column
    ImGui::TableSetColumnIndex(3);
    if (entry.vsizeMB > 0) {
        ImGui::Text("%llu", entry.vsizeMB);
    } else {
        ImGui::TextDisabled("-");
    }
    
    // RSS column
    ImGui::TableSetColumnIndex(4);
    if (entry.rssMB > 0) {
        ImGui::Text("%llu", entry.rssMB);
    } else {
        ImGui::TextDisabled("-");
    }
    
    // Threads column
    ImGui::TableSetColumnIndex(5);
    if (entry.threads > 0) {
        ImGui::Text("%u", entry.threads);
    } else {
        ImGui::TextDisabled("-");
    }
}

void PIDSelector::HandleSelection(uint32_t pid) {
    std::cout << "\n=== PIDSelector::HandleSelection ===\n";
    std::cout << "Selected PID " << pid << " for camera " << selectedCamera << "\n";
    
    // Find the process name
    std::string processName = "Unknown";
    for (const auto& proc : processes) {
        if (proc.pid == pid) {
            processName = proc.name;
            std::cout << "Process name: " << processName << "\n";
            break;
        }
    }
    
    // Set camera focus via beacon
    if (beaconReader) {
        std::cout << "Setting camera focus...\n";
        beaconReader->SetCameraFocus(selectedCamera, pid);
    } else {
        std::cout << "WARNING: No beacon reader available!\n";
    }
    
    // Call selection callback with PID and process name
    if (onSelection) {
        std::cout << "Calling selection callback...\n";
        onSelection(pid, processName);
    } else {
        std::cout << "WARNING: No selection callback set!\n";
    }
    
    std::cout << "=================================\n";
    
    // Hide menu
    Hide();
}

} // namespace Haywire