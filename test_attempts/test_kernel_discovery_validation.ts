#!/usr/bin/env tsx

/**
 * Test script for new kernel discovery validation logic
 * Tests the entry counting approach against the known working kernel PGD at 0x136dbf000
 */

import { promises as fs } from 'fs';
import { PagedKernelDiscovery } from './web/src/kernel-discovery-paged.js';

// Known real kernel PGD from QMP query-kernel-info (TTBR1)
const REAL_SWAPPER_PGD_PA = 0x136dbf000;
const GUEST_RAM_START = 0x40000000;

/**
 * Simple test class that extends PagedKernelDiscovery to access private methods
 */
class KernelDiscoveryTester extends PagedKernelDiscovery {
    // Make private method accessible for testing
    public testPgdEntryCountValidation(pgdOffset: number) {
        return this.testPgdWithEntryCountValidation(pgdOffset);
    }
}

async function main() {
    console.log('=== Testing Kernel Discovery Entry Count Validation ===\n');

    // Check if memory file exists
    const memoryFilePath = '/tmp/haywire-vm-mem';
    try {
        await fs.access(memoryFilePath);
        console.log(`✓ Found memory file at ${memoryFilePath}`);
    } catch (error) {
        console.error(`✗ Memory file not found at ${memoryFilePath}`);
        console.error('Please make sure QEMU VM is running with shared memory backend');
        process.exit(1);
    }

    // Get file size
    const stats = await fs.stat(memoryFilePath);
    const totalSize = stats.size;
    console.log(`Memory file size: ${(totalSize / (1024 * 1024 * 1024)).toFixed(2)}GB\n`);

    // Calculate offset of real PGD in memory file
    const realPgdOffset = REAL_SWAPPER_PGD_PA - GUEST_RAM_START;
    console.log(`Real kernel PGD PA: 0x${REAL_SWAPPER_PGD_PA.toString(16)}`);
    console.log(`Offset in memory file: 0x${realPgdOffset.toString(16)}`);

    // Check if the real PGD offset is within the file
    if (realPgdOffset < 0 || realPgdOffset >= totalSize) {
        console.error(`✗ Real PGD offset is outside memory file range (0-0x${totalSize.toString(16)})`);
        process.exit(1);
    }
    console.log(`✓ Real PGD is within memory file range\n`);

    try {
        // Create kernel discovery instance
        console.log('Creating PagedKernelDiscovery instance...');
        const discovery = new KernelDiscoveryTester(memoryFilePath);

        // Test the real kernel PGD with new validation
        console.log('=== Testing Real Kernel PGD with Entry Count Validation ===');
        const entryCount = discovery.testPgdEntryCountValidation(realPgdOffset);

        console.log('\nResults:');
        console.log(`  Total valid entries: ${entryCount.validEntries}`);
        console.log(`  Kernel entries (indices 256-511): ${entryCount.kernelEntries}`);
        console.log(`  User entries (indices 0-255): ${entryCount.userEntries}`);

        // Check if it passes Python's validation criteria (>50 kernel entries)
        const passesValidation = entryCount.kernelEntries >= 50;
        console.log(`\nValidation result: ${passesValidation ? '✓ PASS' : '✗ FAIL'}`);

        if (passesValidation) {
            console.log(`✓ Real kernel PGD passes entry count validation (${entryCount.kernelEntries} ≥ 50 kernel entries)`);
            console.log('✓ New validation approach successfully identifies the real kernel PGD!');
        } else {
            console.log(`✗ Real kernel PGD fails entry count validation (${entryCount.kernelEntries} < 50 kernel entries)`);
            console.log('✗ New validation approach needs adjustment');
        }

        // Test a few other random offsets to compare
        console.log('\n=== Testing Random Offsets for Comparison ===');
        const randomOffsets = [
            0x1000000,   // 16MB
            0x10000000,  // 256MB
            0x20000000,  // 512MB
        ];

        for (const offset of randomOffsets) {
            if (offset < totalSize - 4096) {
                const randomCount = discovery.testPgdEntryCountValidation(offset);
                const randomAddr = offset + GUEST_RAM_START;
                console.log(`Random offset 0x${randomAddr.toString(16)}: ${randomCount.validEntries} valid, ${randomCount.kernelEntries} kernel`);
            }
        }

    } catch (error) {
        console.error('Error during testing:', error);
        process.exit(1);
    }

    console.log('\n=== Test Complete ===');
}

// Run the test
main().catch(console.error);