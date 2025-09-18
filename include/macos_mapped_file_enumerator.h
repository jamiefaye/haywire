#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <tuple>
#include <cstdint>

namespace Haywire {

struct MappedFileInfo {
    std::string path;
    std::string process_name;
    uint32_t pid = 0;
    uint64_t start_address = 0;
    uint64_t size = 0;
    std::string permissions = "---";  // r/w/x
    bool is_shared = false;
};

class MacOSMappedFileEnumerator {
public:
    MacOSMappedFileEnumerator();
    ~MacOSMappedFileEnumerator() = default;

    // Get all memory-mapped files from user's processes
    std::vector<MappedFileInfo> EnumerateMappedFiles();

    // Get memory-mapped files for a specific process
    std::vector<MappedFileInfo> GetMappedFilesForProcess(uint32_t pid);

    // Get list of user's processes
    std::vector<std::pair<uint32_t, std::string>> GetUserProcesses();

    // Get list of processes that have interesting mapped files (with count)
    std::vector<std::tuple<uint32_t, std::string, size_t>> GetProcessesWithMappedFiles();

    // Filter options
    void SetIncludeSystemLibraries(bool include) { includeSystemLibs = include; }
    void SetIncludeFrameworks(bool include) { includeFrameworks = include; }
    void SetIncludeExecutables(bool include) { includeExecutables = include; }

private:
    // Parse vmmap output for a process (using Mach APIs)
    std::vector<MappedFileInfo> ParseVmmapOutput(uint32_t pid, const std::string& process_name);

    // Fallback to shell-based vmmap if Mach APIs fail
    std::vector<MappedFileInfo> ParseVmmapOutputViaShell(uint32_t pid, const std::string& process_name);

    // Check if file is accessible
    bool IsFileAccessible(const std::string& path);

    // Filter out system paths we probably don't want
    bool ShouldIncludeFile(const std::string& path);

    bool includeSystemLibs = false;
    bool includeFrameworks = false;
    bool includeExecutables = true;

    // Cache of known inaccessible paths to avoid repeated checks
    std::set<std::string> inaccessiblePaths;

    // Cache of process file counts
    mutable std::map<uint32_t, size_t> processFileCountCache;
};

} // namespace Haywire