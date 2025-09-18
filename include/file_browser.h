#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <memory>
#include "imgui.h"
#ifdef __APPLE__
#include "macos_mapped_file_enumerator.h"
#endif

namespace Haywire {

#ifndef __APPLE__
class MacOSMappedFileEnumerator;
struct MappedFileInfo;
#endif

class FileBrowser {
public:
    FileBrowser();
    ~FileBrowser() = default;

    // Open the file browser dialog
    void Open();

    // Close the dialog
    void Close();

    // Draw the file browser (call from ImGui context)
    // Returns true if a file was selected
    bool Draw();

    // Get the selected file path
    const std::string& GetSelectedPath() const { return selectedPath; }

    // Check if the browser is open
    bool IsOpen() const { return isOpen; }

    // Set initial directory
    void SetCurrentDirectory(const std::string& path);

    // Set file extension filter (e.g., ".txt,.cpp,.h")
    void SetTypeFilter(const std::string& filter);

    // Set mode (regular files or memory-mapped files)
    void SetShowMappedFiles(bool show) { showMappedFiles = show; needsRefresh = true; }
    bool IsShowingMappedFiles() const { return showMappedFiles; }

private:
    struct FileEntry {
        std::string name;
        std::string path;
        bool isDirectory;
        size_t size;
        std::filesystem::file_time_type lastModified;
    };

    void RefreshFileList();
    void NavigateToParent();
    void NavigateToDirectory(const std::string& path);
    std::string FormatFileSize(size_t size) const;
    bool MatchesFilter(const std::string& filename) const;

    bool isOpen;
    std::string selectedPath;
    std::string currentDirectory;
    std::string typeFilter;
    std::vector<FileEntry> fileList;
    std::vector<std::string> directoryHistory;
    char inputPath[512];
    int selectedIndex;
    bool needsRefresh;

    // Quick access directories
    std::vector<std::pair<std::string, std::string>> quickAccess;

    // Memory-mapped file support
    bool showMappedFiles;
    std::unique_ptr<MacOSMappedFileEnumerator> mappedFileEnumerator;
    std::vector<MappedFileInfo> mappedFileList;
    void RefreshMappedFileList();
    std::string currentProcessFilter;
    std::string cachedProcessFilter;  // Track what's currently cached
    bool onlyShowProcessesWithFiles = true;  // Filter to only show processes with mapped files
    bool processListInitialized = false;
};

} // namespace Haywire