#include "macos_mapped_file_enumerator.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sstream>
#include <algorithm>
#include <sys/sysctl.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <pwd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace Haywire {

MacOSMappedFileEnumerator::MacOSMappedFileEnumerator() {
}

std::vector<MappedFileInfo> MacOSMappedFileEnumerator::EnumerateMappedFiles() {
    std::vector<MappedFileInfo> allFiles;
    std::set<std::string> uniquePaths;  // Track unique files

    auto processes = GetUserProcesses();
    for (const auto& [pid, name] : processes) {
        auto mappedFiles = GetMappedFilesForProcess(pid);
        for (const auto& file : mappedFiles) {
            // Only add if we haven't seen this file before
            if (uniquePaths.insert(file.path).second) {
                allFiles.push_back(file);
            }
        }
    }

    return allFiles;
}

std::vector<MappedFileInfo> MacOSMappedFileEnumerator::GetMappedFilesForProcess(uint32_t pid) {
    // Get process name
    std::string processName = "Unknown";
    char pathBuffer[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, pathBuffer, sizeof(pathBuffer)) > 0) {
        std::string fullPath(pathBuffer);
        size_t lastSlash = fullPath.rfind('/');
        if (lastSlash != std::string::npos) {
            processName = fullPath.substr(lastSlash + 1);
        }
    }

    return ParseVmmapOutput(pid, processName);
}

std::vector<std::pair<uint32_t, std::string>> MacOSMappedFileEnumerator::GetUserProcesses() {
    std::vector<std::pair<uint32_t, std::string>> userProcesses;

    // Get current user ID
    uid_t currentUID = getuid();

    // Get process list
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_UID, static_cast<int>(currentUID)};
    size_t size = 0;

    // Get required size
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) {
        return userProcesses;
    }

    // Allocate buffer and get process list
    std::vector<uint8_t> buffer(size);
    if (sysctl(mib, 4, buffer.data(), &size, nullptr, 0) != 0) {
        return userProcesses;
    }

    // Parse process list
    size_t count = size / sizeof(struct kinfo_proc);
    struct kinfo_proc* processes = reinterpret_cast<struct kinfo_proc*>(buffer.data());

    for (size_t i = 0; i < count; i++) {
        pid_t pid = processes[i].kp_proc.p_pid;
        std::string name(processes[i].kp_proc.p_comm);

        if (pid > 0 && !name.empty()) {
            userProcesses.push_back({static_cast<uint32_t>(pid), name});
        }
    }

    return userProcesses;
}

std::vector<MappedFileInfo> MacOSMappedFileEnumerator::ParseVmmapOutput(uint32_t pid, const std::string& process_name) {
    std::vector<MappedFileInfo> mappedFiles;

    // Get task port for the process
    task_t task;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        // Fall back to shell vmmap for processes we can't access directly
        // This works for our own user's processes even without task_for_pid
        return ParseVmmapOutputViaShell(pid, process_name);
    }

    // Enumerate memory regions using Mach APIs
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t info_count;

    while (true) {
        info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kr = mach_vm_region_recurse(task, &address, &size, &depth,
                                    (vm_region_info_t)&info, &info_count);

        if (kr != KERN_SUCCESS) {
            break;
        }

        // Check if this region is file-backed
        if (info.object_id != 0) {
            // Get the file path for this region
            char pathname[PROC_PIDPATHINFO_SIZE];
            int ret = proc_regionfilename(pid, address, pathname, sizeof(pathname));

            if (ret > 0 && pathname[0] == '/') {
                std::string path(pathname);

                // Apply filters
                if (ShouldIncludeFile(path) && IsFileAccessible(path)) {
                    MappedFileInfo fileInfo;
                    fileInfo.path = path;
                    fileInfo.process_name = process_name;
                    fileInfo.pid = pid;
                    fileInfo.start_address = address;
                    fileInfo.size = size;

                    // Convert protection to string
                    std::string perms;
                    if (info.protection & VM_PROT_READ) perms += "r";
                    else perms += "-";
                    if (info.protection & VM_PROT_WRITE) perms += "w";
                    else perms += "-";
                    if (info.protection & VM_PROT_EXECUTE) perms += "x";
                    else perms += "-";
                    fileInfo.permissions = perms;

                    fileInfo.is_shared = (info.share_mode == SM_SHARED);

                    mappedFiles.push_back(fileInfo);
                }
            }
        }

        // Move to next region
        address += size;
    }

    mach_port_deallocate(mach_task_self(), task);
    return mappedFiles;
}

std::vector<MappedFileInfo> MacOSMappedFileEnumerator::ParseVmmapOutputViaShell(uint32_t pid, const std::string& process_name) {
    std::vector<MappedFileInfo> mappedFiles;

    // Original shell-based implementation as fallback
    std::string cmd = "vmmap -v " + std::to_string(pid) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return mappedFiles;
    }

    char line[1024];
    bool inRegionSection = false;

    while (fgets(line, sizeof(line), pipe)) {
        std::string lineStr(line);

        if (lineStr.find("REGION ") != std::string::npos || lineStr.find("MALLOC") != std::string::npos) {
            inRegionSection = true;
            continue;
        }

        if (!inRegionSection) continue;
        if (lineStr.length() < 10) continue;

        size_t pathStart = lineStr.rfind("  /");
        if (pathStart == std::string::npos) {
            pathStart = lineStr.find(" /");
            if (pathStart == std::string::npos) continue;
        }

        std::string path = lineStr.substr(pathStart);
        path.erase(0, path.find_first_not_of(" \t\n\r"));
        path.erase(path.find_last_not_of(" \t\n\r") + 1);

        if (path.empty() || path[0] != '/') continue;
        if (!ShouldIncludeFile(path)) continue;
        if (!IsFileAccessible(path)) continue;

        MappedFileInfo info;
        info.path = path;
        info.process_name = process_name;
        info.pid = pid;
        info.start_address = 0;
        info.size = 0;  // Initialize to 0 instead of garbage
        info.permissions = "---";
        info.is_shared = (lineStr.find("SM=") != std::string::npos);

        // Try to parse size from the line (format: [...[ 512K]...])
        size_t sizeStart = lineStr.find("[ ");
        if (sizeStart != std::string::npos) {
            sizeStart += 2;
            size_t sizeEnd = lineStr.find(']', sizeStart);
            if (sizeEnd != std::string::npos) {
                std::string sizeStr = lineStr.substr(sizeStart, sizeEnd - sizeStart);
                // Trim whitespace
                sizeStr.erase(0, sizeStr.find_first_not_of(" \t"));
                sizeStr.erase(sizeStr.find_last_not_of(" \t") + 1);

                // Parse size with suffix (K, M, G)
                if (!sizeStr.empty()) {
                    char suffix = sizeStr.back();
                    if (std::isalpha(suffix)) {
                        double value = std::stod(sizeStr.substr(0, sizeStr.length() - 1));
                        switch (suffix) {
                            case 'K': info.size = static_cast<uint64_t>(value * 1024); break;
                            case 'M': info.size = static_cast<uint64_t>(value * 1024 * 1024); break;
                            case 'G': info.size = static_cast<uint64_t>(value * 1024 * 1024 * 1024); break;
                            default: info.size = static_cast<uint64_t>(value); break;
                        }
                    } else {
                        info.size = std::stoull(sizeStr);
                    }
                }
            }
        }

        mappedFiles.push_back(info);
    }

    pclose(pipe);
    return mappedFiles;
}

bool MacOSMappedFileEnumerator::IsFileAccessible(const std::string& path) {
    // Check cache first
    if (inaccessiblePaths.count(path) > 0) {
        return false;
    }

    // Just check if file exists (F_OK), not if it's readable (R_OK)
    // Many system files are mapped but not directly readable due to SIP
    if (access(path.c_str(), F_OK) != 0) {
        inaccessiblePaths.insert(path);
        return false;
    }

    // For actual file opening in Haywire, we'll focus on files we can read
    // But for listing, show all mapped files that exist
    return true;
}

bool MacOSMappedFileEnumerator::ShouldIncludeFile(const std::string& path) {
    // Exclude /dev
    if (path.find("/dev/") == 0) return false;

    // Always include interesting file types
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find(".qcow2") != std::string::npos) return true;
    if (lower.find(".dmg") != std::string::npos) return true;
    if (lower.find(".iso") != std::string::npos) return true;
    if (lower.find(".img") != std::string::npos) return true;
    if (lower.find(".vdi") != std::string::npos) return true;
    if (lower.find(".vhd") != std::string::npos) return true;
    if (lower.find(".vmdk") != std::string::npos) return true;
    if (lower.find(".sqlite") != std::string::npos) return true;
    if (lower.find(".db") != std::string::npos) return true;
    if (lower.find(".asar") != std::string::npos) return true;
    if (lower.find(".json") != std::string::npos) return true;
    if (lower.find(".plist") != std::string::npos) return true;

    // Include user files
    if (path.find("/Users/") == 0) return true;
    if (path.find("/Applications/") == 0) return true;
    if (path.find("/tmp/") == 0) return true;
    if (path.find("/var/folders/") == 0) return true;
    if (path.find("/private/tmp/") == 0) return true;
    if (path.find("/private/var/") == 0) return true;

    // Skip system libraries and frameworks by default
    return false;

    /* ORIGINAL FILTERING - TOO STRICT
    // Filter out most system stuff unless explicitly requested
    if (!includeSystemLibs) {
        if (path.find("/usr/lib/") == 0) return false;
        if (path.find("/System/Library/") == 0) return false;
        // But allow /Library/Application Support
        if (path.find("/Library/") == 0 &&
            path.find("/Library/Application") != 0) return false;
    }

    // Filter out frameworks unless requested
    if (!includeFrameworks && path.find(".framework/") != std::string::npos) {
        return false;
    }

    // Filter out most dylibs
    if (!includeSystemLibs && path.find(".dylib") != std::string::npos) {
        // But keep application-specific ones
        if (path.find("/Applications/") != 0 &&
            path.find("/Users/") != 0) {
            return false;
        }
    }

    // Prioritize interesting file types
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Always include disk images and databases
    if (lower.find(".qcow2") != std::string::npos) return true;
    if (lower.find(".dmg") != std::string::npos) return true;
    if (lower.find(".iso") != std::string::npos) return true;
    if (lower.find(".img") != std::string::npos) return true;
    if (lower.find(".vdi") != std::string::npos) return true;
    if (lower.find(".vhd") != std::string::npos) return true;
    if (lower.find(".vmdk") != std::string::npos) return true;
    if (lower.find(".sqlite") != std::string::npos) return true;
    if (lower.find(".db") != std::string::npos) return true;
    if (lower.find(".asar") != std::string::npos) return true;

    // Include executables from Applications
    if (path.find("/Applications/") == 0) return true;

    // Include user files
    if (path.find("/Users/") == 0) return true;

    // Include /tmp and /var/folders (temp files, caches)
    if (path.find("/tmp/") == 0) return true;
    if (path.find("/var/folders/") == 0) return true;
    if (path.find("/private/tmp/") == 0) return true;
    if (path.find("/private/var/") == 0) return true;

    return false;
    */
}

/* ORIGINAL NOTES - keeping for reference
    // Filter out system libraries unless requested
    if (!includeSystemLibs) {
        if (path.find("/usr/lib/") == 0) return false;
        if (path.find("/System/Library/") == 0) return false;
        if (path.find("/Library/") == 0 && path.find("/Library/Application") != 0) return false;
    }

    // Filter out frameworks unless requested
    if (!includeFrameworks) {
        if (path.find(".framework/") != std::string::npos) return false;
    }

    // Filter out dylibs unless they're interesting
    if (!includeSystemLibs && path.find(".dylib") != std::string::npos) {
        // But keep application-specific dylibs
        if (path.find("/Applications/") != 0 &&
            path.find("/Users/") != 0) {
            return false;
        }
    }

    // Always exclude these
    if (path.find("/dev/") == 0) return false;
    if (path.find("/.") != std::string::npos) return false;  // Hidden files

    // Focus on interesting file types
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Always include these types
    if (lower.find(".qcow2") != std::string::npos) return true;
    if (lower.find(".dmg") != std::string::npos) return true;
    if (lower.find(".iso") != std::string::npos) return true;
    if (lower.find(".img") != std::string::npos) return true;
    if (lower.find(".vdi") != std::string::npos) return true;
    if (lower.find(".vhd") != std::string::npos) return true;
    if (lower.find(".vmdk") != std::string::npos) return true;
    if (lower.find(".sqlite") != std::string::npos) return true;
    if (lower.find(".db") != std::string::npos) return true;
    if (lower.find(".asar") != std::string::npos) return true;  // Electron apps

    // Include executables from Applications
    if (includeExecutables && path.find("/Applications/") == 0) {
        return true;
    }

    // Include user files
    if (path.find("/Users/") == 0) {
        return true;
    }

    return false;
    */

std::vector<std::tuple<uint32_t, std::string, size_t>> MacOSMappedFileEnumerator::GetProcessesWithMappedFiles() {
    std::vector<std::tuple<uint32_t, std::string, size_t>> result;

    auto processes = GetUserProcesses();

    // For the "only with files" checkbox, we'll just show common processes
    // that typically have interesting files, to avoid scanning all 366 processes
    std::set<std::string> interestingProcesses = {
        "haywire", "qemu-system-aarch64", "qemu-system-x86_64",
        "UTM", "VirtualBuddy", "Docker", "Parallels",
        "Code", "Electron", "Chrome", "Safari", "Firefox",
        "Slack", "Discord", "Telegram", "WhatsApp",
        "Xcode", "lldb", "gdb", "python3", "node", "java"
    };

    for (const auto& [pid, name] : processes) {
        // Only check processes that are likely to have interesting files
        bool shouldCheck = false;
        std::string nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

        for (const auto& interesting : interestingProcesses) {
            std::string interestingLower = interesting;
            std::transform(interestingLower.begin(), interestingLower.end(), interestingLower.begin(), ::tolower);
            if (nameLower.find(interestingLower) != std::string::npos) {
                shouldCheck = true;
                break;
            }
        }

        if (!shouldCheck) continue;

        // Check cache first
        if (processFileCountCache.count(pid) > 0) {
            size_t count = processFileCountCache[pid];
            if (count > 0) {
                result.push_back({pid, name, count});
            }
        } else {
            // Quick scan to count files
            auto files = GetMappedFilesForProcess(pid);
            size_t count = files.size();
            processFileCountCache[pid] = count;
            if (count > 0) {
                result.push_back({pid, name, count});
            }
        }
    }

    // Sort by file count descending
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return std::get<2>(a) > std::get<2>(b);
              });

    return result;
}

} // namespace Haywire