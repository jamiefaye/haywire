#include "file_browser.h"
#include "macos_mapped_file_enumerator.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <pwd.h>
#include <unistd.h>
#include <set>
#include <tuple>

namespace Haywire {

FileBrowser::FileBrowser()
    : isOpen(false), selectedIndex(-1), needsRefresh(true), showMappedFiles(false) {

    // Create mapped file enumerator for macOS
#ifdef __APPLE__
    mappedFileEnumerator = std::make_unique<MacOSMappedFileEnumerator>();
    mappedFileEnumerator->SetIncludeSystemLibraries(false);
    mappedFileEnumerator->SetIncludeFrameworks(false);
    mappedFileEnumerator->SetIncludeExecutables(true);
#endif

    // Get home directory
    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            homeDir = pw->pw_dir;
        }
    }

    // Set initial directory to home or root
    currentDirectory = homeDir ? homeDir : "/";
    std::strcpy(inputPath, currentDirectory.c_str());

    // Set up quick access directories
    if (homeDir) {
        quickAccess.push_back({"Home", homeDir});
        quickAccess.push_back({"Desktop", std::string(homeDir) + "/Desktop"});
        quickAccess.push_back({"Documents", std::string(homeDir) + "/Documents"});
        quickAccess.push_back({"Downloads", std::string(homeDir) + "/Downloads"});
    }
    quickAccess.push_back({"Root", "/"});
    quickAccess.push_back({"Applications", "/Applications"});
    quickAccess.push_back({"/usr/bin", "/usr/bin"});
    quickAccess.push_back({"/usr/local/bin", "/usr/local/bin"});
    quickAccess.push_back({"/bin", "/bin"});

    RefreshFileList();
}

void FileBrowser::Open() {
    isOpen = true;
    selectedIndex = -1;
    selectedPath.clear();
    needsRefresh = true;
}

void FileBrowser::Close() {
    isOpen = false;
}

bool FileBrowser::Draw() {
    if (!isOpen) {
        return false;
    }

    bool fileSelected = false;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    ImGui::Begin("File Browser", &isOpen, ImGuiWindowFlags_NoCollapse);

    // Tab bar for regular files vs memory-mapped files
    if (ImGui::BeginTabBar("FileBrowserTabs")) {
        if (ImGui::BeginTabItem("Regular Files")) {
            if (showMappedFiles) {
                showMappedFiles = false;
                needsRefresh = true;
            }
            ImGui::EndTabItem();
        }

#ifdef __APPLE__
        if (ImGui::BeginTabItem("Memory Mapped Files")) {
            if (!showMappedFiles) {
                showMappedFiles = true;
                needsRefresh = true;
            }
            ImGui::EndTabItem();
        }
#endif
        ImGui::EndTabBar();
    }

    // Top bar with current path and navigation
    if (ImGui::Button("Up")) {
        NavigateToParent();
    }
    ImGui::SameLine();
    if (ImGui::Button("Home")) {
        const char* homeDir = getenv("HOME");
        if (homeDir) {
            NavigateToDirectory(homeDir);
        }
    }
    ImGui::SameLine();

    // Path input (only for regular files mode)
    if (!showMappedFiles) {
        ImGui::PushItemWidth(-1);
        if (ImGui::InputText("##Path", inputPath, sizeof(inputPath),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (std::filesystem::exists(inputPath) &&
                std::filesystem::is_directory(inputPath)) {
                NavigateToDirectory(inputPath);
            }
        }
        ImGui::PopItemWidth();
    } else {
        // Show filter for memory-mapped files
        if (currentProcessFilter.empty()) {
            ImGui::Text("Select a process to view memory-mapped files");
        } else {
            ImGui::Text("Showing files from: %s", currentProcessFilter.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                needsRefresh = true;
            }
        }
    }

    ImGui::Separator();

    // Main content area with two panels
    if (!showMappedFiles) {
        ImGui::BeginChild("QuickAccess", ImVec2(150, 0), true);
        ImGui::Text("Quick Access");
        ImGui::Separator();
        for (const auto& [name, path] : quickAccess) {
            if (std::filesystem::exists(path)) {
                if (ImGui::Selectable(name.c_str(), currentDirectory == path)) {
                    NavigateToDirectory(path);
                }
            }
        }
        ImGui::EndChild();
    } else {
        // Show process list for memory-mapped files
        ImGui::BeginChild("ProcessFilter", ImVec2(200, 0), true);
        ImGui::Text("Processes");
        ImGui::Separator();

        ImGui::TextDisabled("Select a process:");
        ImGui::Separator();

#ifdef __APPLE__
        if (mappedFileEnumerator) {
            // Always show all processes without pre-scanning
            auto processes = mappedFileEnumerator->GetUserProcesses();

            // Filter to interesting processes to reduce clutter
            std::vector<std::pair<uint32_t, std::string>> interestingProcs;
            for (const auto& [pid, name] : processes) {
                std::string nameLower = name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

                // Show processes likely to have interesting files
                if (nameLower.find("qemu") != std::string::npos ||
                    nameLower.find("utm") != std::string::npos ||
                    nameLower.find("haywire") != std::string::npos ||
                    nameLower.find("virtualbuddy") != std::string::npos ||
                    nameLower.find("parallels") != std::string::npos ||
                    nameLower.find("docker") != std::string::npos ||
                    nameLower.find("code") != std::string::npos ||
                    nameLower.find("chrome") != std::string::npos ||
                    nameLower.find("firefox") != std::string::npos ||
                    nameLower.find("safari") != std::string::npos ||
                    nameLower.find("electron") != std::string::npos) {
                    interestingProcs.push_back({pid, name});
                }
            }

            if (interestingProcs.empty()) {
                ImGui::TextDisabled("No VM/browser processes found");
            } else {
                for (const auto& [pid, name] : interestingProcs) {
                    std::string label = name + " (" + std::to_string(pid) + ")";
                    bool isSelected = (currentProcessFilter == name);
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        currentProcessFilter = name;
                        needsRefresh = true;
                    }
                }
            }
        }
#endif
        ImGui::EndChild();
    }

    ImGui::SameLine();

    // File list
    ImGui::BeginChild("FileList", ImVec2(0, -30), true);

    // Refresh if needed
    if (needsRefresh) {
        if (!showMappedFiles) {
            RefreshFileList();
        } else {
            RefreshMappedFileList();
            cachedProcessFilter = currentProcessFilter;
        }
        needsRefresh = false;
    }

    // Table with file details
    int numColumns = showMappedFiles ? 4 : 3;
    if (ImGui::BeginTable("Files", numColumns,
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_Sortable |
                          ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_RowBg)) {

        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        if (showMappedFiles) {
            ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, 300);
        }
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();

        if (!showMappedFiles) {
            // Regular file list
            for (size_t i = 0; i < fileList.size(); i++) {
                const auto& entry = fileList[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                // Icon for directories
                std::string displayName = entry.isDirectory ?
                    "📁 " + entry.name : entry.name;

                bool isSelected = (selectedIndex == static_cast<int>(i));
                if (ImGui::Selectable(displayName.c_str(), isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedIndex = i;

                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (entry.isDirectory) {
                            NavigateToDirectory(entry.path);
                            selectedIndex = -1;
                        } else {
                            selectedPath = entry.path;
                            fileSelected = true;
                            isOpen = false;
                        }
                    }
                }

                ImGui::TableNextColumn();
                if (!entry.isDirectory) {
                    ImGui::Text("%s", FormatFileSize(entry.size).c_str());
                } else {
                    ImGui::Text("-");
                }

                ImGui::TableNextColumn();
                // Convert file_time_type to system time (C++17 compatible)
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    entry.lastModified - std::filesystem::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                auto time = std::chrono::system_clock::to_time_t(sctp);
                char timeStr[100];
                std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M",
                             std::localtime(&time));
                ImGui::Text("%s", timeStr);
            }
        } else {
            // Memory-mapped file list
#ifdef __APPLE__
            for (size_t i = 0; i < mappedFileList.size(); i++) {
                const auto& mf = mappedFileList[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                // Extract just filename from path
                std::string displayName = mf.path;
                size_t lastSlash = displayName.rfind('/');
                if (lastSlash != std::string::npos) {
                    displayName = displayName.substr(lastSlash + 1);
                }

                // Add icon for different file types
                if (mf.path.find(".qcow2") != std::string::npos) {
                    displayName = "💾 " + displayName;
                } else if (mf.path.find(".dmg") != std::string::npos) {
                    displayName = "📀 " + displayName;
                } else if (mf.path.find(".asar") != std::string::npos) {
                    displayName = "📦 " + displayName;
                }

                bool isSelected = (selectedIndex == static_cast<int>(i));
                if (ImGui::Selectable(displayName.c_str(), isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedIndex = i;

                    if (ImGui::IsMouseDoubleClicked(0)) {
                        selectedPath = mf.path;
                        fileSelected = true;
                        isOpen = false;
                    }
                }

                // Tooltip with full path
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", mf.path.c_str());
                }

                ImGui::TableNextColumn();
                ImGui::Text("%s", mf.process_name.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%s", FormatFileSize(mf.size).c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%s", mf.permissions.c_str());
            }
#endif
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();

    // Bottom bar with buttons
    ImGui::Separator();

    if (ImGui::Button("Select", ImVec2(100, 0))) {
        if (selectedIndex >= 0 && selectedIndex < fileList.size()) {
            const auto& entry = fileList[selectedIndex];
            if (!entry.isDirectory) {
                selectedPath = entry.path;
                fileSelected = true;
                isOpen = false;
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        isOpen = false;
    }

    // Display selected file
    if (selectedIndex >= 0) {
        ImGui::SameLine();
        if (!showMappedFiles && selectedIndex < fileList.size()) {
            const auto& entry = fileList[selectedIndex];
            if (!entry.isDirectory) {
                ImGui::Text("Selected: %s", entry.name.c_str());
            }
        }
#ifdef __APPLE__
        else if (showMappedFiles && selectedIndex < mappedFileList.size()) {
            std::string name = mappedFileList[selectedIndex].path;
            size_t lastSlash = name.rfind('/');
            if (lastSlash != std::string::npos) {
                name = name.substr(lastSlash + 1);
            }
            ImGui::Text("Selected: %s", name.c_str());
        }
#endif
    }

    ImGui::End();

    return fileSelected;
}

void FileBrowser::SetCurrentDirectory(const std::string& path) {
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        currentDirectory = path;
        std::strcpy(inputPath, currentDirectory.c_str());
        needsRefresh = true;
    }
}

void FileBrowser::SetTypeFilter(const std::string& filter) {
    typeFilter = filter;
    needsRefresh = true;
}

void FileBrowser::RefreshFileList() {
    fileList.clear();
    selectedIndex = -1;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(currentDirectory)) {
            try {
                FileEntry fe;
                fe.path = entry.path().string();
                fe.name = entry.path().filename().string();
                fe.isDirectory = entry.is_directory();

                if (!fe.isDirectory) {
                    // Check filter if set
                    if (!typeFilter.empty() && !MatchesFilter(fe.name)) {
                        continue;
                    }
                    fe.size = std::filesystem::file_size(entry);
                } else {
                    fe.size = 0;
                }

                fe.lastModified = entry.last_write_time();
                fileList.push_back(fe);
            } catch (const std::exception&) {
                // Skip files we can't access
            }
        }
    } catch (const std::exception&) {
        // Directory not accessible
    }

    // Sort: directories first, then by name
    std::sort(fileList.begin(), fileList.end(),
              [](const FileEntry& a, const FileEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory > b.isDirectory;
        }
        return a.name < b.name;
    });
}

void FileBrowser::NavigateToParent() {
    std::filesystem::path p(currentDirectory);
    if (p.has_parent_path()) {
        NavigateToDirectory(p.parent_path().string());
    }
}

void FileBrowser::NavigateToDirectory(const std::string& path) {
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        currentDirectory = std::filesystem::canonical(path).string();
        std::strcpy(inputPath, currentDirectory.c_str());
        needsRefresh = true;
        selectedIndex = -1;
    }
}

std::string FileBrowser::FormatFileSize(size_t size) const {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unitIndex = 0;
    double displaySize = static_cast<double>(size);

    while (displaySize >= 1024.0 && unitIndex < 3) {
        displaySize /= 1024.0;
        unitIndex++;
    }

    std::stringstream ss;
    if (unitIndex == 0) {
        ss << size << " B";
    } else {
        ss << std::fixed << std::setprecision(1) << displaySize << " " << units[unitIndex];
    }
    return ss.str();
}

bool FileBrowser::MatchesFilter(const std::string& filename) const {
    if (typeFilter.empty()) {
        return true;
    }

    // Parse comma-separated extensions
    std::stringstream ss(typeFilter);
    std::string extension;

    while (std::getline(ss, extension, ',')) {
        // Remove leading/trailing whitespace
        extension.erase(0, extension.find_first_not_of(" \t"));
        extension.erase(extension.find_last_not_of(" \t") + 1);

        if (!extension.empty()) {
            // Add dot if not present
            if (extension[0] != '.') {
                extension = "." + extension;
            }

            // Check if filename ends with this extension
            if (filename.size() >= extension.size()) {
                if (filename.compare(filename.size() - extension.size(),
                                    extension.size(), extension) == 0) {
                    return true;
                }
            }
        }
    }

    return false;
}

void FileBrowser::RefreshMappedFileList() {
    mappedFileList.clear();
    fileList.clear();
    selectedIndex = -1;

#ifdef __APPLE__
    if (!mappedFileEnumerator) {
        fprintf(stderr, "DEBUG: No mappedFileEnumerator!\n");
        return;
    }

    fprintf(stderr, "DEBUG: RefreshMappedFileList called, filter='%s'\n", currentProcessFilter.c_str());

    // Only scan if a specific process is selected
    // Don't scan all processes at once (too slow)
    if (!currentProcessFilter.empty()) {
        // Find process by name and get its files
        auto processes = mappedFileEnumerator->GetUserProcesses();
        fprintf(stderr, "DEBUG: Found %zu processes\n", processes.size());

        for (const auto& [pid, name] : processes) {
            fprintf(stderr, "DEBUG: Checking process '%s' (pid %d) against filter '%s'\n",
                    name.c_str(), pid, currentProcessFilter.c_str());
            if (name == currentProcessFilter) {
                fprintf(stderr, "DEBUG: Match! Getting files for pid %d\n", pid);
                mappedFileList = mappedFileEnumerator->GetMappedFilesForProcess(pid);
                fprintf(stderr, "DEBUG: Got %zu files\n", mappedFileList.size());
                break;
            }
        }
    }
    // If no process selected, mappedFileList stays empty
    // User must select a process first

    // Remove duplicates (same file mapped multiple times)
    std::set<std::string> uniquePaths;
    auto it = mappedFileList.begin();
    while (it != mappedFileList.end()) {
        if (!uniquePaths.insert(it->path).second) {
            it = mappedFileList.erase(it);
        } else {
            ++it;
        }
    }

    // Sort by filename
    std::sort(mappedFileList.begin(), mappedFileList.end(),
              [](const MappedFileInfo& a, const MappedFileInfo& b) {
                  size_t slashA = a.path.rfind('/');
                  size_t slashB = b.path.rfind('/');
                  std::string nameA = slashA != std::string::npos ?
                                       a.path.substr(slashA + 1) : a.path;
                  std::string nameB = slashB != std::string::npos ?
                                       b.path.substr(slashB + 1) : b.path;
                  return nameA < nameB;
              });
#endif
}

} // namespace Haywire