#!/usr/bin/env node

import fs from 'fs';
import { exec } from 'child_process';

// Get vmalloc range from kernel
async function getVmallocRange() {
    return new Promise((resolve) => {
        exec(`ssh vm "sudo grep -E 'vmalloc|vmemmap|modules' /proc/vmallocinfo | head -5"`, (error, stdout) => {
            if (!error && stdout) {
                console.log('Sample /proc/vmallocinfo entries:');
                console.log(stdout);
            }
        });
        
        // Also check for vmalloc symbols
        exec(`ssh vm "sudo grep -E 'vmalloc_start|vmalloc_end|VMALLOC' /proc/kallsyms | head -10"`, (error, stdout) => {
            if (!error && stdout) {
                console.log('\nVmalloc-related symbols:');
                console.log(stdout);
            }
            resolve();
        });
    });
}

await getVmallocRange();

// Now analyze our mapping data
console.log('\n' + '='.repeat(70) + '\n');
console.log('Analyzing kernel virtual address regions from page tables:');
console.log('\nPGD[256] (0x0000800000000000+) contains:');
console.log('  PUD[1] at VA 0x800040000000 (256GB offset)');
console.log('  PUD[2] at VA 0x800080000000 (512GB offset)');
console.log('');

// ARM64 Linux kernel VA layout (typical):
console.log('Typical ARM64 kernel virtual memory layout:');
console.log('============================================');
console.log('0xffff000000000000 - 0xffff7fffffffffff : User space (47-bit)');
console.log('0xffff800000000000 - 0xffff80007fffffff : Kernel linear map (512GB)');
console.log('0xffff800080000000 - 0xfffffdffbffeffff : vmalloc region');
console.log('0xfffffdffbfff0000 - 0xfffffdfffe5f8fff : Fixed mappings');
console.log('0xfffffe0000000000 - 0xffffffffffffffff : Kernel modules');
console.log('');
console.log('Our findings:');
console.log('------------');
console.log('PGD[256] VA base: 0x0000800000000000');
console.log('  This is NOT the standard 0xffff800000000000!');
console.log('  This kernel uses a different VA layout.');
console.log('');
console.log('The sparse mappings we see at:');
console.log('  0x800040000000 - might be vmalloc area');
console.log('  0x800080000000 - might be another vmalloc region');
console.log('');
console.log('Most vmalloc allocations are demand-paged, so they only');
console.log('appear in page tables when actually allocated and accessed.');
console.log('This explains the sparse mappings.');