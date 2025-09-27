#!/usr/bin/env node
/**
 * Test finding open files through process file tables in Electron
 * Run with: node test-open-files-electron.mjs
 */

import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { readFileSync } from 'fs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Import the modules we need
async function testOpenFiles() {
    console.log('=== Testing Open Files Discovery ===\n');

    // Dynamically import the modules
    const { PagedMemory } = await import('./dist/paged-memory.js');
    const { PageCacheDiscovery } = await import('./dist/page-cache-discovery.js');
    const { PagedKernelDiscovery } = await import('./dist/kernel-discovery-paged.js');

    // Open the memory file
    const memoryFile = '/tmp/haywire-vm-mem';
    console.log(`Opening memory file: ${memoryFile}`);

    // Create file handle (simplified for Node.js)
    const fileData = readFileSync(memoryFile);
    const fakeFile = {
        arrayBuffer: async () => fileData.buffer,
        name: memoryFile,
        size: fileData.length
    };

    const memory = new PagedMemory(fakeFile);
    await memory.initialize();

    console.log(`Loaded memory file: ${(memory.fileSize / (1024*1024*1024)).toFixed(2)} GB\n`);

    // Create kernel discovery
    const kernelDiscovery = new PagedKernelDiscovery(memory);

    // Find kernel PGD
    console.log('Finding kernel PGD...');
    const pgdResult = await kernelDiscovery.findKernelPgd();
    if (!pgdResult.found) {
        console.error('Failed to find kernel PGD');
        return;
    }

    console.log(`Kernel PGD found at: 0x${pgdResult.pgdPA.toString(16)}\n`);

    // Create page cache discovery with kernel discovery
    const discovery = new PageCacheDiscovery(memory, pgdResult.pgdPA, undefined, kernelDiscovery);

    // Test the new open files discovery
    console.log('Discovering open files through process file tables...');
    const openFiles = await discovery.discoverOpenFiles();

    console.log(`\n=== Results ===`);
    console.log(`Found ${openFiles.size} unique open files\n`);

    // Show details for all discovered files
    if (openFiles.size > 0) {
        console.log('All discovered open files:\n');

        // Convert to array and sort by inode number
        const filesArray = Array.from(openFiles.values()).sort((a, b) => {
            if (a.ino < b.ino) return -1;
            if (a.ino > b.ino) return 1;
            return 0;
        });

        for (const info of filesArray) {
            console.log(`Inode #${info.ino}:`);
            console.log(`  Address: 0x${info.inodeAddr.toString(16)}`);
            console.log(`  Size: ${info.size} bytes`);
            console.log(`  Open by: ${info.processes.length} process(es)`);
            for (const proc of info.processes) {
                console.log(`    - ${proc.name} (PID ${proc.pid}) on fd ${proc.fd}`);
            }
            console.log();
        }

        // Check if we found the known files
        const knownInodes = [272643, 262017, 267562]; // libpcre2-8.so, bash, systemctl
        console.log('Checking for known files:');
        for (const ino of knownInodes) {
            const found = filesArray.find(f => f.ino === BigInt(ino));
            if (found) {
                console.log(`  ✓ Found inode ${ino}`);
            } else {
                console.log(`  ✗ Missing inode ${ino}`);
            }
        }
    }
}

// Run the test
testOpenFiles().catch(console.error);