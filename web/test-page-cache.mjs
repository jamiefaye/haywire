#!/usr/bin/env node
/**
 * Test page cache discovery in Node.js environment
 * Run with: node test-page-cache.mjs
 */

import fs from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// We'll need to create a simple PagedMemory implementation for Node
class SimplePagedMemory {
    constructor(filePath) {
        this.filePath = filePath;
        this.fd = fs.openSync(filePath, 'r');
        this.stats = fs.fstatSync(this.fd);
        this.fileSize = this.stats.size;
    }

    readBytes(offset, size) {
        if (offset < 0 || offset >= this.fileSize) {
            return null;
        }
        
        const actualSize = Math.min(size, this.fileSize - offset);
        const buffer = Buffer.alloc(actualSize);
        
        try {
            fs.readSync(this.fd, buffer, 0, actualSize, offset);
            return new Uint8Array(buffer);
        } catch (e) {
            console.error(`Error reading at offset ${offset}:`, e);
            return null;
        }
    }

    readU64(offset) {
        const data = this.readBytes(offset, 8);
        if (!data || data.length < 8) return null;
        
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        return view.getBigUint64(0, true);
    }

    readU32(offset) {
        const data = this.readBytes(offset, 4);
        if (!data || data.length < 4) return 0;
        
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        return view.getUint32(0, true);
    }

    close() {
        fs.closeSync(this.fd);
    }
}

// Simple page cache discovery test
async function testPageCacheDiscovery() {
    const memoryFile = '/tmp/haywire-vm-mem';
    
    if (!fs.existsSync(memoryFile)) {
        console.error(`Memory file not found: ${memoryFile}`);
        console.error('Make sure the VM is running with memory-backend-file');
        return;
    }

    console.log(`Opening memory file: ${memoryFile}`);
    const memory = new SimplePagedMemory(memoryFile);
    
    try {
        console.log(`File size: ${(memory.fileSize / (1024*1024*1024)).toFixed(2)} GB\n`);

        // Test finding super_blocks
        console.log('=== Testing Page Cache Discovery ===\n');
        
        // Try known super_blocks addresses
        const superBlocksAddresses = [
            0xffff8000838e3360,  // Our test system address
            0xffffffff82a3e4d0,  // Common on 5.15
        ];

        for (const addr of superBlocksAddresses) {
            console.log(`Trying super_blocks at VA 0x${addr.toString(16)}`);
            
            // Simple linear map translation
            let pa = null;
            if (addr >= 0xffff800000000000 && addr < 0xffff800080000000) {
                // Linear map: PA = VA - 0xffff800000000000 + 0x40000000
                pa = addr - 0xffff800000000000 + 0x40000000;
                console.log(`  Linear map -> PA 0x${pa.toString(16)}`);
            } else {
                console.log(`  Not in linear map, skipping`);
                continue;
            }

            // Convert PA to file offset
            const offset = pa - 0x40000000;
            console.log(`  File offset: 0x${offset.toString(16)}`);
            
            // Try to read the list_head
            const data = memory.readBytes(offset, 16);
            if (data && data.length === 16) {
                const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
                const next = view.getBigUint64(0, true);
                const prev = view.getBigUint64(8, true);
                
                console.log(`  list_head.next: 0x${next.toString(16)}`);
                console.log(`  list_head.prev: 0x${prev.toString(16)}`);
                
                // Check if they look like kernel pointers
                if (next > BigInt('0xffff000000000000') && prev > BigInt('0xffff000000000000')) {
                    console.log(`  ✓ Looks valid!\n`);
                    
                    // Try to walk the list
                    await walkSuperBlocks(memory, addr, offset);
                } else {
                    console.log(`  ✗ Doesn't look like valid kernel pointers\n`);
                }
            } else {
                console.log(`  ✗ Could not read data\n`);
            }
        }

        // Try scanning for superblock signatures
        console.log('\n=== Scanning for Superblock Signatures ===\n');
        scanForSuperblocks(memory);
        
    } finally {
        memory.close();
    }
}

async function walkSuperBlocks(memory, headVA, headOffset) {
    console.log('=== Walking Superblock List ===\n');
    
    const S_LIST_OFFSET = 0x30;  // super_block.s_list offset
    const maxEntries = 10;
    const entries = [];
    
    // Read first next pointer
    const headData = memory.readBytes(headOffset, 8);
    if (!headData) {
        console.log('Could not read head data');
        return;
    }
    
    let current = new DataView(headData.buffer, headData.byteOffset, headData.byteLength).getBigUint64(0, true);
    const first = current;
    let count = 0;
    
    while (current && Number(current) !== headVA && count < maxEntries) {
        count++;
        
        // Container_of: subtract list offset to get struct address  
        const sbAddr = Number(current) - S_LIST_OFFSET;
        console.log(`Superblock #${count} at VA 0x${sbAddr.toString(16)}`);
        
        // Try to read s_id (filesystem identifier)
        const S_ID_OFFSET = 0x440;
        const idVA = sbAddr + S_ID_OFFSET;
        
        // Translate to PA
        if (idVA >= 0xffff800000000000 && idVA < 0xffff800080000000) {
            const idPA = idVA - 0xffff800000000000 + 0x40000000;
            const idOffset = idPA - 0x40000000;
            
            const idData = memory.readBytes(idOffset, 32);
            if (idData) {
                let id = '';
                for (let i = 0; i < idData.length && idData[i] !== 0; i++) {
                    if (idData[i] >= 32 && idData[i] < 127) {
                        id += String.fromCharCode(idData[i]);
                    }
                }
                if (id) {
                    console.log(`  Filesystem ID: "${id}"`);
                }
            }
        }
        
        // Get next in list
        const currentPA = Number(current) - 0xffff800000000000 + 0x40000000;
        const currentOffset = currentPA - 0x40000000;
        const nextData = memory.readBytes(currentOffset, 8);
        
        if (!nextData) break;
        
        current = new DataView(nextData.buffer, nextData.byteOffset, nextData.byteLength).getBigUint64(0, true);
        
        // Check if we've looped
        if (current === first) {
            console.log('\nReached end of list');
            break;
        }
    }
    
    console.log(`\nFound ${count} superblocks`);
}

function scanForSuperblocks(memory) {
    const EXT4_MAGIC = 0xEF53;
    const TMPFS_MAGIC = 0x01021994;
    const PROC_SUPER_MAGIC = 0x9fa0;
    const SYSFS_MAGIC = 0x62656572;
    
    console.log('Scanning first 10MB for filesystem magic numbers...');
    
    let found = 0;
    for (let offset = 0; offset < 10 * 1024 * 1024 && found < 5; offset += 4096) {
        // Check various offsets where magic might be
        const magicOffsets = [0x38, 0x3c, 0x40, 0x44, 0x48];
        
        for (const mOffset of magicOffsets) {
            const magic = memory.readU32(offset + mOffset);
            
            if (magic === EXT4_MAGIC) {
                console.log(`  Found EXT4 magic at offset 0x${offset.toString(16)} + 0x${mOffset.toString(16)}`);
                found++;
            } else if (magic === TMPFS_MAGIC) {
                console.log(`  Found TMPFS magic at offset 0x${offset.toString(16)} + 0x${mOffset.toString(16)}`);
                found++;
            } else if (magic === PROC_SUPER_MAGIC) {
                console.log(`  Found PROC magic at offset 0x${offset.toString(16)} + 0x${mOffset.toString(16)}`);
                found++;
            } else if (magic === SYSFS_MAGIC) {
                console.log(`  Found SYSFS magic at offset 0x${offset.toString(16)} + 0x${mOffset.toString(16)}`);
                found++;
            }
        }
    }
    
    if (found === 0) {
        console.log('  No filesystem signatures found');
    }
}

// Run the test
testPageCacheDiscovery().catch(console.error);
