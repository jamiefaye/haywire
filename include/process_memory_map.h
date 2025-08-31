#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "guest_agent.h"

namespace Haywire {

// Visualizes a process's memory layout, skipping sparse regions
class ProcessMemoryMap {
public:
    ProcessMemoryMap();
    ~ProcessMemoryMap();
    
    // Load memory map for a process
    void LoadProcess(int pid, GuestAgent* agent);
    
    // Draw the memory map overview
    void Draw();
    
    // Get selected region info
    bool GetSelectedRegion(uint64_t& start, uint64_t& end) const;
    
private:
    struct MemorySegment {
        uint64_t start;
        uint64_t end;
        std::string name;
        std::string permissions;
        
        enum Type {
            CODE,      // Executable code
            DATA,      // Data sections
            HEAP,      // Heap
            STACK,     // Stack
            LIBRARY,   // Shared libraries
            MMAP,      // Memory mapped regions
            ANON,      // Anonymous mappings
            VDSO,      // Kernel provided
            UNKNOWN
        };
        Type type;
        
        void DetermineType() {
            if (permissions.find('x') != std::string::npos) {
                type = CODE;
            } else if (name == "[heap]") {
                type = HEAP;
            } else if (name == "[stack]" || name.find("stack") != std::string::npos) {
                type = STACK;
            } else if (name.find(".so") != std::string::npos) {
                type = LIBRARY;
            } else if (name == "[vdso]" || name == "[vvar]" || name == "[vsyscall]") {
                type = VDSO;
            } else if (name.empty() || name[0] == '[') {
                type = ANON;
            } else if (name[0] == '/') {
                type = MMAP;
            } else {
                type = UNKNOWN;
            }
        }
        
        bool IsInteresting() const {
            // Skip kernel-provided regions and small anonymous mappings
            if (type == VDSO) return false;
            if (type == ANON && (end - start) < 1024*1024) return false; // Skip small anon
            return true;
        }
        
        const char* GetTypeName() const {
            switch(type) {
                case CODE: return "Code";
                case DATA: return "Data";
                case HEAP: return "Heap";
                case STACK: return "Stack";
                case LIBRARY: return "Library";
                case MMAP: return "Mapped File";
                case ANON: return "Anonymous";
                case VDSO: return "Kernel";
                default: return "Unknown";
            }
        }
        
        uint32_t GetTypeColor() const {
            switch(type) {
                case CODE: return 0xFF4444FF;     // Red
                case DATA: return 0xFF44FF44;     // Green
                case HEAP: return 0xFFFFFF44;     // Yellow
                case STACK: return 0xFFFF44FF;    // Magenta
                case LIBRARY: return 0xFF44FFFF;  // Cyan
                case MMAP: return 0xFFFF8844;     // Orange
                case ANON: return 0xFF888888;     // Gray
                case VDSO: return 0xFF444444;     // Dark gray
                default: return 0xFFCCCCCC;       // Light gray
            }
        }
    };
    
    int currentPid;
    std::vector<MemorySegment> segments;
    int selectedIndex;
    
    // Layout calculation
    struct LayoutGroup {
        std::string name;
        uint64_t startAddr;
        uint64_t endAddr;
        std::vector<size_t> segmentIndices;
        float displayY;      // Y position in overview
        float displayHeight; // Height in overview
    };
    std::vector<LayoutGroup> layoutGroups;
    
    void CalculateLayout();
    void GroupSegments();
};

}