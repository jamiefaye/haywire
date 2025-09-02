#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <cstdint>

int main() {
    const char* mem_file = "/tmp/haywire-vm-mem";
    const size_t mem_size = 4ULL * 1024 * 1024 * 1024; // 4GB
    
    // PIDs to search for
    std::vector<uint32_t> target_pids = {2291, 1493, 2114, 1681, 2075};
    
    // Open memory file
    int fd = open(mem_file, O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << mem_file << std::endl;
        return 1;
    }
    
    // Map the memory
    void* mem = mmap(nullptr, mem_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mem == MAP_FAILED) {
        std::cerr << "Failed to mmap" << std::endl;
        close(fd);
        return 1;
    }
    
    std::cout << "Searching 4GB memory for PIDs: ";
    for (auto pid : target_pids) {
        std::cout << pid << " ";
    }
    std::cout << std::endl;
    
    uint8_t* data = (uint8_t*)mem;
    std::vector<std::pair<uint32_t, uint64_t>> found_pids;
    
    // Search through memory
    for (uint64_t offset = 0; offset < mem_size - 4; offset += 4) {
        uint32_t value = *(uint32_t*)(data + offset);
        
        for (auto target_pid : target_pids) {
            if (value == target_pid) {
                found_pids.push_back({target_pid, offset});
                
                if (found_pids.size() <= 20) {  // Show first 20 matches
                    std::cout << "Found PID " << target_pid << " at offset 0x" 
                              << std::hex << offset << std::dec << std::endl;
                    
                    // Check for nearby strings that might indicate this is a task_struct
                    // Look for comm field nearby (within 2KB)
                    for (int delta = -0x800; delta < 0x800; delta += 16) {
                        if (offset + delta >= 0 && offset + delta < mem_size - 16) {
                            char* str = (char*)(data + offset + delta);
                            
                            // Check if it contains process-related strings
                            if (strstr(str, "vlc") || strstr(str, "gnome") || 
                                strstr(str, "mutter") || strstr(str, "Xwayland")) {
                                // Make sure it's printable
                                bool printable = true;
                                for (int i = 0; i < 15 && str[i]; i++) {
                                    if (str[i] < 32 || str[i] > 126) {
                                        printable = false;
                                        break;
                                    }
                                }
                                
                                if (printable && strlen(str) > 0 && strlen(str) < 16) {
                                    std::cout << "  Nearby string at +" << std::hex << delta 
                                              << ": '" << str << "'" << std::dec << std::endl;
                                    
                                    // This might be the comm field - calculate task_struct start
                                    // Common comm offsets: 0x5C8, 0x738, 0x4E8
                                    for (int comm_offset : {0x5C8, 0x738, 0x4E8}) {
                                        uint64_t potential_task = offset + delta - comm_offset;
                                        if (potential_task > 0 && potential_task < mem_size) {
                                            // Check if there's a valid PID at the expected offset
                                            uint32_t pid_check = *(uint32_t*)(data + potential_task + 0x398);
                                            if (pid_check == target_pid) {
                                                std::cout << "  *** Likely task_struct at 0x" << std::hex 
                                                          << potential_task << std::dec 
                                                          << " (PID at +0x398, comm at +0x" << std::hex 
                                                          << comm_offset << ")" << std::dec << std::endl;
                                                
                                                // Check for list pointers
                                                uint64_t* list_ptr = (uint64_t*)(data + potential_task + 0x398);
                                                uint64_t next_va = list_ptr[0];
                                                uint64_t prev_va = list_ptr[1];
                                                
                                                if ((next_va >> 48) == 0xffff && (prev_va >> 48) == 0xffff) {
                                                    std::cout << "    tasks.next: 0x" << std::hex << next_va << std::dec << std::endl;
                                                    std::cout << "    tasks.prev: 0x" << std::hex << prev_va << std::dec << std::endl;
                                                    
                                                    if (next_va != prev_va) {
                                                        std::cout << "    *** HAS ACTIVE PROCESS LIST! ***" << std::endl;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    std::cout << "\nTotal matches found: " << found_pids.size() << std::endl;
    
    // Summary by PID
    std::cout << "\nSummary by PID:" << std::endl;
    for (auto target_pid : target_pids) {
        int count = 0;
        for (auto& p : found_pids) {
            if (p.first == target_pid) count++;
        }
        if (count > 0) {
            std::cout << "  PID " << target_pid << ": " << count << " occurrences" << std::endl;
        }
    }
    
    munmap(mem, mem_size);
    close(fd);
    return 0;
}