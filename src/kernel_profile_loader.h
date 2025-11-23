#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

// Simple JSON parser for kernel profiles (no external dependencies)
// Only parses the specific structure we need

struct KernelProfile {
    std::string name;
    std::string kernel_version;
    std::string architecture;
    std::string os;  // "linux" or "windows"

    // Linux: task_struct offsets
    size_t task_pid = 888;
    size_t task_tgid = 892;
    size_t task_comm = 1144;
    size_t task_mm = 1064;
    size_t task_tasks = 1664;
    size_t task_size = 9088;

    // Linux: mm_struct offsets
    size_t mm_pgd = 0x68;
    size_t mm_mt = 0x40;
    size_t mm_users = 0x74;

    // Linux: vm_area_struct offsets
    size_t vma_start = 0x00;
    size_t vma_end = 0x08;
    size_t vma_next = 0x10;
    size_t vma_flags = 0x20;
    size_t vma_file = 0x80;

    // Windows: EPROCESS offsets
    size_t eprocess_unique_process_id = 1088;
    size_t eprocess_active_process_links = 1096;
    size_t eprocess_image_file_name = 1448;
    size_t eprocess_vad_root = 1560;
    size_t eprocess_peb = 1608;
    size_t eprocess_size = 2816;

    // Windows: KPROCESS offsets (within EPROCESS)
    size_t kprocess_directory_table_base = 40;

    // Windows: MMVAD offsets
    size_t mmvad_short_starting_vpn = 24;
    size_t mmvad_short_ending_vpn = 28;
    size_t mmvad_short_vad_flags = 48;

    bool verified = false;
};

class KernelProfileLoader {
public:
    // Load profile from JSON file
    static bool LoadProfile(const std::string& filepath, KernelProfile& profile) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Failed to open profile: " << filepath << std::endl;
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        // Simple JSON parsing - look for specific keys
        profile.name = ExtractString(content, "\"name\":");
        profile.kernel_version = ExtractString(content, "\"kernel_version\":");
        profile.os = ExtractString(content, "\"os\":");
        profile.architecture = ExtractString(content, "\"architecture\":");
        profile.verified = (content.find("\"verified\": true") != std::string::npos);

        // Determine OS type
        bool isWindows = (profile.os == "windows");

        if (isWindows) {
            // Extract Windows offsets
            profile.eprocess_unique_process_id = ExtractOffset(content, "\"UniqueProcessId\":", "\"offset\":");
            profile.eprocess_active_process_links = ExtractOffset(content, "\"ActiveProcessLinks\":", "\"offset\":");
            profile.eprocess_image_file_name = ExtractOffset(content, "\"ImageFileName\":", "\"offset\":");
            profile.eprocess_vad_root = ExtractOffset(content, "\"VadRoot\":", "\"offset\":");
            profile.eprocess_peb = ExtractOffset(content, "\"Peb\":", "\"offset\":");
            profile.eprocess_size = ExtractInt(content, "\"EPROCESS\":", "\"size\":");

            profile.kprocess_directory_table_base = ExtractOffset(content, "\"DirectoryTableBase\":", "\"offset\":");

            profile.mmvad_short_starting_vpn = ExtractOffset(content, "\"StartingVpn\":", "\"offset\":");
            profile.mmvad_short_ending_vpn = ExtractOffset(content, "\"EndingVpn\":", "\"offset\":");
            profile.mmvad_short_vad_flags = ExtractOffset(content, "\"VadFlags\":", "\"offset\":");
        } else {
            // Extract Linux offsets
            profile.task_pid = ExtractOffset(content, "\"pid\":", "\"offset\":");
            profile.task_tgid = ExtractOffset(content, "\"tgid\":", "\"offset\":");
            profile.task_comm = ExtractOffset(content, "\"comm\":", "\"offset\":");
            profile.task_mm = ExtractOffset(content, "\"mm\":", "\"offset\":");
            profile.task_tasks = ExtractOffset(content, "\"tasks\":", "\"offset\":");
            profile.task_size = ExtractInt(content, "\"task_struct\":", "\"size\":");

            profile.mm_pgd = ExtractOffset(content, "\"pgd\":", "\"offset\":");
            profile.mm_mt = ExtractOffset(content, "\"mm_mt\":", "\"offset\":");
            profile.mm_users = ExtractOffset(content, "\"mm_users\":", "\"offset\":");

            profile.vma_start = ExtractOffset(content, "\"vm_start\":", "\"offset\":");
            profile.vma_end = ExtractOffset(content, "\"vm_end\":", "\"offset\":");
            profile.vma_next = ExtractOffset(content, "\"vm_next\":", "\"offset\":");
            profile.vma_flags = ExtractOffset(content, "\"vm_flags\":", "\"offset\":");
            profile.vma_file = ExtractOffset(content, "\"vm_file\":", "\"offset\":");
        }

        std::cout << "Loaded kernel profile: " << profile.name << std::endl;
        std::cout << "  OS: " << profile.os << std::endl;
        std::cout << "  Kernel: " << profile.kernel_version << " (" << profile.architecture << ")" << std::endl;
        std::cout << "  Verified: " << (profile.verified ? "yes" : "no") << std::endl;

        return true;
    }

    // Try to auto-detect profile based on available files and architecture
    static std::string DetectProfile(const std::string& profilesDir) {
        // Detect architecture
        std::string arch = "unknown";
        #if defined(__x86_64__) || defined(_M_X64)
            arch = "x86_64";
        #elif defined(__aarch64__) || defined(_M_ARM64)
            arch = "aarch64";
        #endif

        // Try to find matching profile - check newer versions first
        if (arch == "x86_64") {
            std::string x86Profile = profilesDir + "/ubuntu-6.14.0-35-x86_64.json";
            std::ifstream test(x86Profile);
            if (test.good()) {
                return x86Profile;
            }
        }

        // Fallback to ARM64 - try latest version first
        std::string arm64Profile = profilesDir + "/ubuntu-6.14.0-36-arm64.json";
        std::ifstream test(arm64Profile);
        if (test.good()) {
            return arm64Profile;
        }

        // Fallback to older version
        return profilesDir + "/ubuntu-6.14.0-34-arm64.json";
    }

private:
    // Extract string value after key
    static std::string ExtractString(const std::string& json, const std::string& key) {
        size_t pos = json.find(key);
        if (pos == std::string::npos) return "";

        pos = json.find("\"", pos + key.length());
        if (pos == std::string::npos) return "";

        size_t end = json.find("\"", pos + 1);
        if (end == std::string::npos) return "";

        return json.substr(pos + 1, end - pos - 1);
    }

    // Extract integer value after key (within a parent section)
    static size_t ExtractInt(const std::string& json, const std::string& section, const std::string& key) {
        size_t sectionPos = json.find(section);
        if (sectionPos == std::string::npos) return 0;

        size_t pos = json.find(key, sectionPos);
        if (pos == std::string::npos) return 0;

        pos = json.find(":", pos + key.length());
        if (pos == std::string::npos) return 0;

        // Skip whitespace
        while (pos < json.length() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;

        // Parse number
        size_t value = 0;
        while (pos < json.length() && json[pos] >= '0' && json[pos] <= '9') {
            value = value * 10 + (json[pos] - '0');
            pos++;
        }

        return value;
    }

    // Extract offset field (look for parent field first, then offset within it)
    static size_t ExtractOffset(const std::string& json, const std::string& field, const std::string& key) {
        size_t fieldPos = json.find(field);
        if (fieldPos == std::string::npos) return 0;

        // Find the offset within this field section (next 250 chars to be safe)
        std::string section = json.substr(fieldPos, 250);

        // Look specifically for "offset": to avoid finding "size" first
        size_t pos = section.find("\"offset\":");
        if (pos == std::string::npos) return 0;

        pos += 9;  // Skip past "offset":

        // Skip whitespace
        while (pos < section.length() && (section[pos] == ':' || section[pos] == ' ' || section[pos] == '\t')) pos++;

        // Parse number (support decimal and hex)
        size_t value = 0;
        if (section[pos] == '0' && (section[pos+1] == 'x' || section[pos+1] == 'X')) {
            // Hex number
            pos += 2;
            while (pos < section.length()) {
                char c = section[pos];
                if (c >= '0' && c <= '9') {
                    value = value * 16 + (c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    value = value * 16 + (c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    value = value * 16 + (c - 'A' + 10);
                } else {
                    break;
                }
                pos++;
            }
        } else {
            // Decimal number
            while (pos < section.length() && section[pos] >= '0' && section[pos] <= '9') {
                value = value * 10 + (section[pos] - '0');
                pos++;
            }
        }

        return value;
    }
};
