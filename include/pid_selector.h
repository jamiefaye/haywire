#pragma once

#include <vector>
#include <string>
#include <functional>
#include "beacon_reader.h"
#include "imgui.h"

namespace Haywire {

class PIDSelector {
public:
    PIDSelector();
    ~PIDSelector();
    
    // Initialize with beacon reader
    void SetBeaconReader(std::shared_ptr<BeaconReader> reader) {
        beaconReader = reader;
    }
    
    // Update PID list from beacon
    void RefreshPIDList();
    
    // Draw the PID selection menu
    void Draw();
    
    // Show/hide the menu
    void Show() { isVisible = true; RefreshPIDList(); }
    void Hide() { isVisible = false; }
    bool IsVisible() const { return isVisible; }
    void ToggleVisible() { isVisible = !isVisible; if (isVisible) RefreshPIDList(); }
    
    // Get selected PID
    uint32_t GetSelectedPID() const { return selectedPID; }
    
    // Selection callback - now includes process name
    using SelectionCallback = std::function<void(uint32_t pid, const std::string& processName)>;
    void SetSelectionCallback(SelectionCallback cb) { onSelection = cb; }
    
private:
    // Display entry for each process
    struct ProcessDisplayEntry {
        uint32_t pid;
        uint32_t ppid;
        std::string name;
        std::string exe;
        char state;
        uint64_t vsizeMB;
        uint64_t rssMB;
        uint32_t threads;
        bool hasDetails;
        
        // For display and sorting
        std::string GetStateString() const {
            switch(state) {
                case 'R': return "Running";
                case 'S': return "Sleeping";
                case 'D': return "Disk Sleep";
                case 'Z': return "Zombie";
                case 'T': return "Stopped";
                case 'X': return "Dead";
                default: return "Unknown";
            }
        }
        
        uint32_t GetStateColor() const {
            switch(state) {
                case 'R': return 0xFF00FF00;  // Green for running
                case 'S': return 0xFFCCCCCC;  // Gray for sleeping
                case 'D': return 0xFFFFFF00;  // Yellow for disk sleep
                case 'Z': return 0xFFFF0000;  // Red for zombie
                case 'T': return 0xFFFF8800;  // Orange for stopped
                default: return 0xFF888888;   // Dark gray
            }
        }
    };
    
    // UI state
    bool isVisible;
    uint32_t selectedPID;
    int selectedCamera;  // 1 or 2
    
    // Process list
    std::vector<ProcessDisplayEntry> processes;
    std::vector<uint32_t> pidList;  // Raw PID list
    
    // Filtering and sorting
    char filterText[256];
    enum SortColumn {
        SORT_PID,
        SORT_NAME,
        SORT_STATE,
        SORT_VSIZE,
        SORT_RSS,
        SORT_THREADS
    };
    SortColumn sortColumn;
    bool sortAscending;
    
    // Display options
    bool showKernelThreads;
    bool showOnlyWithDetails;
    
    // Beacon reader
    std::shared_ptr<BeaconReader> beaconReader;
    
    // Selection callback
    SelectionCallback onSelection;
    
    // Helper functions
    void SortProcessList();
    bool PassesFilter(const ProcessDisplayEntry& entry) const;
    void DrawProcessRow(const ProcessDisplayEntry& entry, bool isSelected);
    void HandleSelection(uint32_t pid);
};

} // namespace Haywire