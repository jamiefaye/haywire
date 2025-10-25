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

    // task_struct offsets
    size_t task_pid = 888;
    size_t task_tgid = 892;
    size_t task_comm = 1144;
    size_t task_mm = 1064;
    size_t task_tasks = 1664;
    size_t task_size = 9088;

    // mm_struct offsets
    size_t mm_pgd = 0x68;
    size_t mm_mt = 0x40;
    size_t mm_users = 0x74;

    // vm_area_struct offsets
    size_t vma_start = 0x00;
    size_t vma_end = 0x08;
    size_t vma_next = 0x10;
    size_t vma_flags = 0x20;
    size_t vma_file = 0x80;

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
        profile.architecture = ExtractString(content, "\"architecture\":");
        profile.verified = (content.find("\"verified\": true") != std::string::npos);

        // Extract offsets
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

        std::cout << "Loaded kernel profile: " << profile.name << std::endl;
        std::cout << "  Kernel: " << profile.kernel_version << " (" << profile.architecture << ")" << std::endl;
        std::cout << "  Verified: " << (profile.verified ? "yes" : "no") << std::endl;

        return true;
    }

    // Try to auto-detect profile based on kernel banner from memory
    static std::string DetectProfile(const std::string& profilesDir) {
        // Future: scan memory for kernel banner and match against profile detection patterns
        // For now, return default
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

        // Find the offset within this field section (next 200 chars)
        std::string section = json.substr(fieldPos, 200);
        size_t pos = section.find(key);
        if (pos == std::string::npos) return 0;

        pos = section.find(":", pos + key.length());
        if (pos == std::string::npos) return 0;

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
