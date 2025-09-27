#!/usr/bin/env node

// Test simple translation by stripping 0xffff prefix
const mm_structs = [
    0xffff0000c9fe8a00n,  // Xwayland
    0xffff0000027fee00n,  // update-notifier
    0xffff0000cb4dc600n,  // goa-identity-se
    0xffff0000027fbc00n,  // goa-daemon
    0xffff0000c8911400n,  // gsd-disk-utilit
];

console.log('Testing simple translation by masking out 0xffff prefix:\n');

for (const va of mm_structs) {
    // Method 1: AND with 0x0000FFFFFFFFFFFF to remove top 16 bits
    const method1 = va & 0x0000FFFFFFFFFFFFn;

    // Method 2: Just take lower 48 bits
    const method2 = va & 0xFFFFFFFFFFFFn;

    // Method 3: Take lower 32 bits and add to guest RAM start
    const method3 = Number(va & 0xFFFFFFFFn) + 0x40000000;

    console.log(`VA: 0x${va.toString(16)}`);
    console.log(`  Method 1 (mask 48 bits):  0x${method1.toString(16)} (${(Number(method1) / (1024*1024*1024)).toFixed(2)} GB)`);
    console.log(`  Method 2 (lower 48 bits):  0x${method2.toString(16)} (${(Number(method2) / (1024*1024*1024)).toFixed(2)} GB)`);
    console.log(`  Method 3 (lower 32 + RAM): 0x${method3.toString(16)} (${(method3 / (1024*1024*1024)).toFixed(2)} GB)`);
    console.log('');
}

// Also check what the broken translation was producing
console.log('Broken translation was producing:');
console.log('  0x5f7974740a00 (383.48 GB)');
console.log('  0x5f7974740e00 (383.48 GB)');
console.log('  0x5f7974740600 (383.48 GB)');