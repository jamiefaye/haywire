/**
 * Quick test for kernel profile loader
 */

#include <iostream>
#include "src/kernel_profile_loader.h"

int main() {
    std::cout << "=== Kernel Profile Loader Test ===" << std::endl;

    KernelProfile profile;
    std::string profilePath = "profiles/ubuntu-6.14.0-34-arm64.json";

    std::cout << "Loading profile: " << profilePath << std::endl;

    if (KernelProfileLoader::LoadProfile(profilePath, profile)) {
        std::cout << "\n✓ Profile loaded successfully!" << std::endl;
        std::cout << "\nProfile Info:" << std::endl;
        std::cout << "  Name: " << profile.name << std::endl;
        std::cout << "  Kernel: " << profile.kernel_version << std::endl;
        std::cout << "  Architecture: " << profile.architecture << std::endl;
        std::cout << "  Verified: " << (profile.verified ? "yes" : "no") << std::endl;

        std::cout << "\ntask_struct offsets:" << std::endl;
        std::cout << "  pid:   " << std::dec << profile.task_pid << " (0x" << std::hex << profile.task_pid << ")" << std::endl;
        std::cout << "  comm:  " << std::dec << profile.task_comm << " (0x" << std::hex << profile.task_comm << ")" << std::endl;
        std::cout << "  mm:    " << std::dec << profile.task_mm << " (0x" << std::hex << profile.task_mm << ")" << std::endl;
        std::cout << "  tasks: " << std::dec << profile.task_tasks << " (0x" << std::hex << profile.task_tasks << ")" << std::endl;

        std::cout << "\nmm_struct offsets:" << std::endl;
        std::cout << "  pgd:      " << std::dec << profile.mm_pgd << " (0x" << std::hex << profile.mm_pgd << ")" << std::endl;
        std::cout << "  mm_mt:    " << std::dec << profile.mm_mt << " (0x" << std::hex << profile.mm_mt << ")" << std::endl;
        std::cout << "  mm_users: " << std::dec << profile.mm_users << " (0x" << std::hex << profile.mm_users << ")" << std::endl;

        std::cout << "\nvm_area_struct offsets:" << std::endl;
        std::cout << "  vm_start: " << std::dec << profile.vma_start << " (0x" << std::hex << profile.vma_start << ")" << std::endl;
        std::cout << "  vm_end:   " << std::dec << profile.vma_end << " (0x" << std::hex << profile.vma_end << ")" << std::endl;
        std::cout << "  vm_flags: " << std::dec << profile.vma_flags << " (0x" << std::hex << profile.vma_flags << ")" << std::endl;
        std::cout << "  vm_file:  " << std::dec << profile.vma_file << " (0x" << std::hex << profile.vma_file << ")" << std::endl;

        return 0;
    } else {
        std::cerr << "\n✗ Failed to load profile" << std::endl;
        return 1;
    }
}
