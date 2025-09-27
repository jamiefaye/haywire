/**
 * Page Cache Discovery for Haywire
 * Discovers unmapped pages in the kernel's page cache
 */

import { PagedMemory } from './paged-memory';
import { PagedKernelDiscovery } from './kernel-discovery-paged';
import { KernelMem, OFFSETS as KMEM_OFFSETS } from './kernel-mem';

// Kernel struct offsets - from pahole/BTF
interface KernelOffsets {
    'super_block.s_list': number;
    'super_block.s_inodes': number;
    'super_block.s_type': number;
    'super_block.s_id': number;
    'inode.i_sb': number;
    'inode.i_sb_list': number;
    'inode.i_mapping': number;
    'inode.i_ino': number;
    'inode.i_size': number;
    'address_space.i_pages': number;
    'address_space.nrpages': number;
    'xarray.xa_head': number;
    'xa_node.shift': number;
    'xa_node.count': number;
    'xa_node.slots': number;
    'list_head.next': number;
    'list_head.prev': number;
    'file_system_type.name': number;
    // Process and file offsets from pahole
    'task_struct.files': number;
    'task_struct.comm': number;
    'task_struct.pid': number;
    'task_struct.mm': number;
    'task_struct.tasks': number;
    'files_struct.fdt': number;
    'files_struct.count': number;
    'fdtable.fd': number;
    'fdtable.max_fds': number;
    'file.f_inode': number;
    'file.f_path': number;
    'file.f_pos': number;
}

// Default offsets for Ubuntu 6.8 kernel
const DEFAULT_OFFSETS: KernelOffsets = {
    'super_block.s_list': 0x00,     // 0 - correct!
    'super_block.s_inodes': 0x548,  // 1352 - correct!
    'super_block.s_type': 0x28,     // 40 - correct!
    'super_block.s_id': 0x3C0,      // 960 - FIXED!
    'inode.i_sb': 0x28,         // 40 in decimal - pointer to super_block
    'inode.i_sb_list': 0x110,  // 272 in decimal
    'inode.i_mapping': 0x30,   // 48 in decimal
    'inode.i_ino': 0x40,        // 64 in decimal
    'inode.i_size': 0x50,       // 80 in decimal
    'address_space.i_pages': 0x08,   // xarray at offset 8
    'address_space.nrpages': 0x58,   // 88 in decimal
    'xarray.xa_head': 0x0,
    'xa_node.shift': 0x8,
    'xa_node.count': 0x1,
    'xa_node.slots': 0x40,
    'list_head.next': 0x0,
    'list_head.prev': 0x8,
    'file_system_type.name': 0x0,
    // Process and file offsets from pahole
    'task_struct.files': 0x9B8,     // 2488 - from pahole
    'task_struct.comm': 0x758,      // 1880 - process name
    'task_struct.pid': 0x4E8,       // 1256 - PID
    'task_struct.mm': 0x998,        // 2456 - mm_struct pointer
    'task_struct.tasks': 0x508,     // 1288 - task list
    'files_struct.fdt': 0x20,       // 32 - fdtable pointer
    'files_struct.count': 0x00,     // 0 - atomic count
    'fdtable.fd': 0x08,             // 8 - file descriptor array
    'fdtable.max_fds': 0x00,        // 0 - max fds
    'file.f_inode': 0x28,           // 40 - inode pointer
    'file.f_path': 0x10,            // 16 - file path
    'file.f_pos': 0x68,             // 104 - file position
};

export interface CachedFile {
    inode: number;
    size: number;
    cachedPages: number;
    cacheSize: number;
    filesystem: string;
}

export interface FileSystem {
    address: number;
    type: string;
    id: string;
    inodes: number;
    cachedPages: number;
}

export interface PageCacheDiscoveryResult {
    totalCachedPages: number;
    totalCacheSize: number;
    filesystems: FileSystem[];
    cachedFiles: CachedFile[];
    discoveredAt: Date;
}

export class PageCacheDiscovery {
    private memory: PagedMemory;
    private offsets: KernelOffsets;
    private superBlocksAddr: number = 0;
    private kernelPgd: number = 0;
    private kernelDiscovery: PagedKernelDiscovery | null = null;
    private kmem: KernelMem;

    constructor(memory: PagedMemory, kernelPgd?: number, offsets?: KernelOffsets, kernelDiscovery?: PagedKernelDiscovery) {
        this.memory = memory;
        this.kernelPgd = kernelPgd || 0;
        this.offsets = offsets || DEFAULT_OFFSETS;
        this.kernelDiscovery = kernelDiscovery || null;

        // Create KernelMem helper
        this.kmem = new KernelMem(memory);
        if (kernelPgd) {
            this.kmem.setKernelPgd(kernelPgd);
        }
        if (kernelPgd) {
            this.kmem.setKernelPgd(kernelPgd);
        }
    }

    /**
     * Find the super_blocks list head address
     */
    private async findSuperBlocks(): Promise<number> {
        // First try the actual super_blocks symbol address
        // Use string and BigInt to preserve precision
        const superBlocksVA = BigInt('0xffff8000838e3360'); // Known from kallsyms
        console.log(`Trying super_blocks at VA 0x${superBlocksVA.toString(16)}`);

        console.log(`Using kernel PGD: 0x${this.kernelPgd.toString(16)}`);
        const pa = this.kmem.translateVA(superBlocksVA);
        if (pa) {
            console.log(`Translated to PA 0x${pa.toString(16)}`);
            // Verify it looks like a list_head
            const fileOffset = pa - 0x40000000;
            const data = this.memory.readBytes(fileOffset, 16);
            if (data) {
                const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
                const next = view.getBigUint64(0, true);
                const prev = view.getBigUint64(8, true);
                console.log(`  super_blocks list: next=0x${next.toString(16)}, prev=0x${prev.toString(16)}`);

                if (next > BigInt('0xffff000000000000') && prev > BigInt('0xffff000000000000')) {
                    console.log('  ✓ Found valid super_blocks list head!');
                    return fileOffset; // Return file offset for walking
                }
            }
        } else {
            console.log('  Could not translate super_blocks VA');
        }

        // Fallback to discovered superblock from scan
        console.log('Falling back to discovered superblock from scan...');

        // Actually scan for the magic number
        const magicOffset = this.scanForMagicNumber();
        if (!magicOffset) {
            console.log('Could not find filesystem magic number');
            return 0;
        }

        console.log(`Found magic at offset 0x${magicOffset.toString(16)}`);

        // Try different s_magic offsets to find struct start
        const magicOffsets = [0x60, 0x10, 0x38, 0x3c, 0x40, 0x44]; // Common offsets for s_magic (0x60 first for our system)

        for (const sMagicOffset of magicOffsets) {
            const sbStart = magicOffset - sMagicOffset;
            if (sbStart < 0) continue;

            // Check if s_list looks valid (at offset 0x30)
            const listOffset = sbStart + this.offsets['super_block.s_list'];
            const listData = this.memory.readBytes(listOffset, 16);

            console.log(`  Trying struct start at 0x${sbStart.toString(16)} (magic at +0x${sMagicOffset.toString(16)}), s_list at 0x${listOffset.toString(16)}`);

            if (listData) {
                const view = new DataView(listData.buffer, listData.byteOffset, listData.byteLength);
                const next = view.getBigUint64(0, true);
                const prev = view.getBigUint64(8, true);

                console.log(`    s_list: next=0x${next.toString(16)}, prev=0x${prev.toString(16)}`);

                // Check if they look like kernel VAs (0xffff...) or reasonable PAs
                // Allow one to be null (empty list or single element)
                const nextValid = next === BigInt(0) ||
                    (next > BigInt('0xffff000000000000')) ||
                    (next > BigInt('0x40000000') && next < BigInt('0x200000000'));

                const prevValid = prev === BigInt(0) ||
                    (prev > BigInt('0xffff000000000000')) ||
                    (prev > BigInt('0x40000000') && prev < BigInt('0x200000000'));

                // At least one should be non-zero and valid
                const looksValid = (nextValid && prevValid) && (next !== BigInt(0) || prev !== BigInt(0));

                if (looksValid) {
                    console.log(`  ✓ Found valid super_block at offset 0x${sbStart.toString(16)} (magic at +0x${sMagicOffset.toString(16)})`);
                    console.log(`    s_list.next: 0x${next.toString(16)}`);
                    console.log(`    s_list.prev: 0x${prev.toString(16)}`);

                    // This is a super_block with the list embedded at offset 0
                    // We need to walk FROM this list to find other superblocks
                    // Return the file offset of the s_list, not the VA
                    return listOffset;  // Use the file offset directly
                } else {
                    console.log(`    Not valid: next doesn't look like a pointer`);
                }
            } else {
                console.log(`    Could not read list data`);
            }
        }

        console.log('Could not find valid super_block structure');
        return 0;
    }

    /**
     * Scan for filesystem magic numbers
     */
    private scanForMagicNumber(): number {
        const EXT4_MAGIC = 0xEF53;
        const TMPFS_MAGIC = 0x01021994;
        const PROC_SUPER_MAGIC = 0x9fa0;
        const SYSFS_MAGIC = 0x62656572;

        // Look for specific known location first
        const knownTmpfs = 0x1bcf060;
        const tmpfsMagic = this.memory.readU32(knownTmpfs);
        if (tmpfsMagic === TMPFS_MAGIC) {
            console.log('Using known TMPFS location');
            return knownTmpfs;
        }

        // Scan first 50MB
        for (let offset = 0; offset < 50 * 1024 * 1024; offset += 4) {
            const magic = this.memory.readU32(offset);

            // Prefer TMPFS and EXT4 over PROC
            if (magic === TMPFS_MAGIC || magic === EXT4_MAGIC) {
                return offset;
            }
        }

        // If no TMPFS/EXT4 found, look for others
        for (let offset = 0; offset < 50 * 1024 * 1024; offset += 4) {
            const magic = this.memory.readU32(offset);

            if (magic === PROC_SUPER_MAGIC || magic === SYSFS_MAGIC) {
                return offset;
            }
        }

        return 0;
    }

    /**
     * Helper wrapper for VA translation that uses KernelMem
     */
    private translateKernelVA(va: number | bigint): number | null {
        const vaBig = typeof va === 'bigint' ? va : BigInt(va);
        return this.kmem.translateVA(vaBig);
    }



    /**
     * Walk a kernel linked list
     */
    private walkList(headAddr: number, listOffset: number, maxEntries: number = 100): bigint[] {
        const entries: bigint[] = [];

        console.log(`Walking list from 0x${headAddr.toString(16)} with offset 0x${listOffset.toString(16)}`);

        // If headAddr is already a file offset (< 0x40000000), use it directly
        // Otherwise try to translate it
        let headFileOffset: number;
        if (headAddr < 0x40000000) {
            headFileOffset = headAddr;
        } else {
            // Use BigInt to preserve precision when translating kernel VAs
            const headPA = this.translateKernelVA(BigInt(headAddr));
            if (!headPA) return entries;
            headFileOffset = headPA - 0x40000000;
        }

        // Read first next pointer
        const headData = this.memory.readBytes(headFileOffset, 8);
        if (!headData) return entries;

        let current = new DataView(headData.buffer, headData.byteOffset, headData.byteLength).getBigUint64(0, true);
        const first = current;

        console.log(`  First entry in list: 0x${current.toString(16)}`);

        // Check if list is empty (points to itself)
        // For a list head at VA headAddr, if the first entry equals headAddr, the list is empty
        if (Number(current) === headAddr) {
            console.log('  List is empty (points to itself)');
            return entries;
        }

        // Special case for tmpfs
        if (current === BigInt('0xffff000001bcf000')) {
            console.log('  List points to itself - this is the tmpfs super_block');
            // Add this superblock itself
            entries.push(BigInt(0x1bcf000));
            return entries;
        }

        while (current && entries.length < maxEntries) {
            // If current looks like a kernel VA (high bits set), need to translate
            // If it's a low address, it might be a file offset already
            let currentFileOffset: number;

            if (current > BigInt(0x40000000)) {
                // Looks like it needs translation - keep as BigInt!
                const pa = this.translateKernelVA(current);  // Pass BigInt directly
                if (!pa) {
                    // Can't translate, might be a physical address already
                    if (current < BigInt(0x200000000)) { // Reasonable PA range
                        currentFileOffset = Number(current - BigInt(0x40000000));
                    } else {
                        break; // Can't handle this address
                    }
                } else {
                    currentFileOffset = pa - 0x40000000;
                }
            } else {
                // Already a file offset
                currentFileOffset = Number(current);
            }

            // Container_of: subtract list offset to get struct address
            const structAddr = current - BigInt(listOffset);
            entries.push(structAddr);

            // Read next pointer
            const nextData = this.memory.readBytes(currentFileOffset, 8);
            if (!nextData) break;

            const next = new DataView(nextData.buffer, nextData.byteOffset, nextData.byteLength).getBigUint64(0, true);

            // Check if we've looped
            if (next === first || next === current) break;

            current = next;
        }

        return entries;
    }

    /**
     * Main discovery function
     */
    public async discover(): Promise<PageCacheDiscoveryResult> {
        console.log('=== Page Cache Discovery ===');
        
        const result: PageCacheDiscoveryResult = {
            totalCachedPages: 0,
            totalCacheSize: 0,
            filesystems: [],
            cachedFiles: [],
            discoveredAt: new Date()
        };
        
        // Step 1: Find super_blocks
        this.superBlocksAddr = await this.findSuperBlocks();
        if (!this.superBlocksAddr) {
            console.log('Could not find super_blocks list');
            return result;
        }
        
        // Step 2: Walk superblock list
        const superblocks = this.walkList(
            this.superBlocksAddr,
            this.offsets['super_block.s_list'],
            50  // Increased limit to find real filesystems like ext4
        );
        
        console.log(`Found ${superblocks.length} mounted filesystems`);

        // Also try discovering open files through process file tables
        console.log('\n=== ALSO: Trying to discover open files through process file tables ===');
        await this.discoverOpenFiles();
        
        // Step 3: Process each superblock
        console.log(`DEBUG offsets object:`, this.offsets);
        for (const sbAddrBig of superblocks) {
            // Keep address as BigInt to avoid precision loss
            console.log(`\n=== Processing superblock at 0x${sbAddrBig.toString(16)} ===`);
            console.log(`sbAddrBig type: ${typeof sbAddrBig}, value: ${sbAddrBig}`);
            console.log(`s_type offset: ${this.offsets['super_block.s_type']} (type: ${typeof this.offsets['super_block.s_type']})`);
            console.log(`s_id offset: ${this.offsets['super_block.s_id']} (type: ${typeof this.offsets['super_block.s_id']})`);
            console.log(`Expected s_type VA: 0x${(sbAddrBig + BigInt(this.offsets['super_block.s_type'])).toString(16)} (BIGINT)`);
            console.log(`Expected s_id VA: 0x${(sbAddrBig + BigInt(this.offsets['super_block.s_id'])).toString(16)} (BIGINT)`);

            const fs: FileSystem = {
                address: Number(sbAddrBig),  // Only convert for display
                type: 'unknown',
                id: '',
                inodes: 0,
                cachedPages: 0
            };
            
            // Try to read filesystem type (s_type field -> file_system_type.name)
            const typeVA = sbAddrBig + BigInt(this.offsets['super_block.s_type']);
            console.log(`  Reading s_type from VA 0x${typeVA.toString(16)} (superblock 0x${sbAddrBig.toString(16)} + offset 0x${this.offsets['super_block.s_type'].toString(16)})`);
            const typePA = this.kmem.translateVA(typeVA);
            console.log(`  s_type VA->PA: 0x${typeVA.toString(16)} -> ${typePA ? '0x' + typePA.toString(16) : 'null'}`);
            if (typePA) {
                const typeData = this.memory.readBytes(typePA - 0x40000000, 8);
                if (typeData) {
                    const typePtr = new DataView(typeData.buffer, typeData.byteOffset, typeData.byteLength).getBigUint64(0, true);
                    console.log(`  s_type pointer: 0x${typePtr.toString(16)}`);

                    // The file_system_type.name field is at offset 0, and it contains a pointer to the name string
                    console.log(`  Translating file_system_type VA 0x${typePtr.toString(16)}`);
                    const namePA = this.kmem.translateVA(typePtr);
                    console.log(`  file_system_type VA->PA: 0x${typePtr.toString(16)} -> ${namePA ? '0x' + namePA.toString(16) : 'null'}`);
                    if (namePA) {
                        // Read the pointer to the name string
                        const namePtrData = this.memory.readBytes(namePA - 0x40000000, 8);
                        if (namePtrData) {
                            const namePtr = new DataView(namePtrData.buffer, namePtrData.byteOffset, namePtrData.byteLength).getBigUint64(0, true);
                            console.log(`  name pointer: 0x${namePtr.toString(16)}`);

                            // Now read the actual name string
                            console.log(`  Translating name string VA 0x${namePtr.toString(16)}`);
                            const nameStrPA = this.kmem.translateVA(namePtr);
                            console.log(`  name string VA->PA: 0x${namePtr.toString(16)} -> ${nameStrPA ? '0x' + nameStrPA.toString(16) : 'null'}`);
                            if (nameStrPA) {
                                const nameData = this.memory.readBytes(nameStrPA - 0x40000000, 32);
                                if (nameData) {
                                    let fsType = '';
                                    for (let i = 0; i < nameData.length && nameData[i] !== 0; i++) {
                                        if (nameData[i] >= 32 && nameData[i] < 127) {
                                            fsType += String.fromCharCode(nameData[i]);
                                        }
                                    }
                                    fs.type = fsType;
                                    console.log(`  Filesystem type identified: "${fsType}"`);
                                }
                            }
                        }
                    }
                }
            }

            // Try to read filesystem ID (s_id field - this is inline, not a pointer)
            const idVA = sbAddrBig + BigInt(this.offsets['super_block.s_id']);
            console.log(`  Reading s_id from VA 0x${idVA.toString(16)} (superblock 0x${sbAddrBig.toString(16)} + offset 0x${this.offsets['super_block.s_id'].toString(16)})`);
            const idPA = this.kmem.translateVA(idVA);
            console.log(`  s_id VA->PA: 0x${idVA.toString(16)} -> ${idPA ? '0x' + idPA.toString(16) : 'null'}`);
            if (idPA) {
                const idData = this.memory.readBytes(idPA - 0x40000000, 32);
                if (idData) {
                    let id = '';
                    for (let i = 0; i < idData.length && idData[i] !== 0; i++) {
                        if (idData[i] >= 32 && idData[i] < 127) {
                            id += String.fromCharCode(idData[i]);
                        }
                    }
                    fs.id = id;
                    if (id) {
                        console.log(`  Filesystem ID: "${id}"`);
                        // vda2 would be the ext4 root filesystem
                        if (id.includes('vda') || id.includes('sda')) {
                            console.log(`  *** This looks like a disk-based filesystem ***`);
                        }
                    }
                }
            }

            // Log filesystem details
            console.log(`\nFilesystem at 0x${sbAddrBig.toString(16)}:`);
            console.log(`  Type: ${fs.type}`);
            console.log(`  ID: ${fs.id}`);

            // DEBUGGING: Only process ext4 for now
            const isExt4 = fs.type === 'ext4';

            if (!isExt4) {
                console.log(`  Skipping ${fs.type} - only debugging ext4 for now`);
                result.filesystems.push(fs);
                continue;
            }

            console.log(`  *** Processing EXT4 filesystem - this should have real file inodes ***`);
            
            // Walk inode list for this superblock
            console.log(`  DEBUG: sbAddrBig=${sbAddrBig}, s_inodes offset=${this.offsets['super_block.s_inodes']}, sbAddrBig type=${typeof sbAddrBig}`);
            const inodeListHead = sbAddrBig + BigInt(this.offsets['super_block.s_inodes']);
            console.log(`  s_inodes list head at: 0x${inodeListHead.toString(16)}`);
            console.log(`  VERIFICATION: ${sbAddrBig} + ${this.offsets['super_block.s_inodes']} = ${inodeListHead}`);

            const inodes = this.walkList(
                Number(inodeListHead),  // walkList expects number for now
                this.offsets['inode.i_sb_list'],
                2000  // Increased limit to see more inodes
            );

            fs.inodes = inodes.length;
            console.log(`  Found ${inodes.length} inodes in this filesystem`);

            let activeInodeCount = 0;
            let skippedInodeCount = 0;

            // Check each inode for cached pages
            for (const inodeAddr of inodes) {
                // Debug: Let's see what we're getting for ext4
                if (isExt4 && inodes.length > 0 && inodes.indexOf(inodeAddr) < 10) {  // Show first 10 inodes for ext4
                    console.log(`  Inode address from walkList: 0x${inodeAddr.toString(16)}`);

                    // IMPORTANT: Let's verify the inode structure layout
                    // The walkList already did container_of, so inodeAddr should be the START of the inode struct

                    // First, let's read the i_sb_list at offset 0x110 to verify it's a valid list_head
                    const listPA = this.translateKernelVA(inodeAddr + BigInt(0x110));
                    if (listPA) {
                        const listData = this.memory.readBytes(listPA - 0x40000000, 16);
                        if (listData) {
                            const next = new DataView(listData.buffer, listData.byteOffset, 8).getBigUint64(0, true);
                            const prev = new DataView(listData.buffer, listData.byteOffset + 8, 8).getBigUint64(0, true);
                            console.log(`    i_sb_list (offset 0x110): next=0x${next.toString(16)}, prev=0x${prev.toString(16)}`);
                        }
                    }

                    console.log(`    Checking inode fields:`);

                    // i_sb at offset from our offsets
                    const sbPA = this.translateKernelVA(inodeAddr + BigInt(this.offsets['inode.i_sb']));
                    if (sbPA) {
                        const sbData = this.memory.readBytes(sbPA - 0x40000000, 8);
                        if (sbData) {
                            const sbPtr = new DataView(sbData.buffer, sbData.byteOffset, sbData.byteLength).getBigUint64(0, true);
                            console.log(`      i_sb (offset 0x${this.offsets['inode.i_sb'].toString(16)}): 0x${sbPtr.toString(16)} (should point back to superblock at 0x${sbAddrBig.toString(16)})`);
                            if (sbPtr === sbAddrBig) {
                                console.log(`        ✓ i_sb correctly points to superblock!`);
                            }
                        }
                    }

                    // i_mapping at offset 48 decimal (0x30 hex)
                    const mapPA = this.translateKernelVA(inodeAddr + BigInt(0x30));
                    if (mapPA) {
                        const mapData = this.memory.readBytes(mapPA - 0x40000000, 8);
                        if (mapData) {
                            const mapPtr = new DataView(mapData.buffer, mapData.byteOffset, mapData.byteLength).getBigUint64(0, true);
                            console.log(`      i_mapping (offset 0x30/48dec): 0x${mapPtr.toString(16)}`);
                        }
                    }

                    // i_ino at offset 64 decimal (0x40 hex)
                    const inoPA = this.translateKernelVA(inodeAddr + BigInt(0x40));
                    if (inoPA) {
                        const inoData = this.memory.readBytes(inoPA - 0x40000000, 8);
                        if (inoData) {
                            const ino = new DataView(inoData.buffer, inoData.byteOffset, inoData.byteLength).getBigUint64(0, true);
                            console.log(`      i_ino (offset 0x40/64dec): ${ino} (0x${ino.toString(16)})`);
                            if (ino < BigInt(1000000)) {
                                console.log(`        ^^ This looks like a real inode number!`);
                            }
                        }
                    }

                    // Let's also check i_mode at offset 0x00 to see if it's a valid file mode
                    const modePA = this.translateKernelVA(inodeAddr);
                    if (modePA) {
                        const modeData = this.memory.readBytes(modePA - 0x40000000, 2);
                        if (modeData) {
                            const mode = modeData[0] | (modeData[1] << 8);
                            console.log(`      i_mode (offset 0x00): 0x${mode.toString(16)}`);
                            // Check if it's a valid mode (S_IFREG=0x8000, S_IFDIR=0x4000, etc)
                            if ((mode & 0xF000) !== 0) {
                                const typeStr = (mode & 0x8000) ? 'regular file' :
                                               (mode & 0x4000) ? 'directory' :
                                               (mode & 0x2000) ? 'char device' :
                                               (mode & 0x6000) ? 'block device' :
                                               (mode & 0x1000) ? 'fifo' :
                                               (mode & 0xA000) ? 'symlink' :
                                               (mode & 0xC000) ? 'socket' : 'unknown';
                                console.log(`        File type: ${typeStr}, permissions: ${(mode & 0x1FF).toString(8)}`);
                            }
                        }
                    }
                }

                // Quick check: Read i_sb to see if this inode is active
                // Skip inodes with NULL i_sb as they're unallocated
                const sbCheckPA = this.translateKernelVA(inodeAddr + BigInt(this.offsets['inode.i_sb']));
                if (sbCheckPA) {
                    const sbCheckData = this.memory.readBytes(sbCheckPA - 0x40000000, 8);
                    if (sbCheckData) {
                        const sbPtr = new DataView(sbCheckData.buffer, sbCheckData.byteOffset, sbCheckData.byteLength).getBigUint64(0, true);
                        if (sbPtr === BigInt(0)) {
                            skippedInodeCount++;
                            continue;  // Skip unallocated inodes
                        }
                    }
                }

                // Get i_mapping pointer
                const mappingPA = this.translateKernelVA(inodeAddr + BigInt(this.offsets['inode.i_mapping']));
                if (!mappingPA) {
                    skippedInodeCount++;
                    continue;
                }

                const mappingData = this.memory.readBytes(mappingPA - 0x40000000, 8);
                if (!mappingData) {
                    skippedInodeCount++;
                    continue;
                }

                const mappingAddr = new DataView(mappingData.buffer, mappingData.byteOffset, mappingData.byteLength).getBigUint64(0, true);
                if (!mappingAddr) {
                    skippedInodeCount++;
                    continue;
                }

                activeInodeCount++;
                
                // Get number of cached pages
                // mappingAddr is already a BigInt, don't convert to Number!
                const nrpagesPA = this.translateKernelVA(mappingAddr + BigInt(this.offsets['address_space.nrpages']));
                if (!nrpagesPA) continue;
                
                const nrpagesData = this.memory.readBytes(nrpagesPA - 0x40000000, 8);
                if (!nrpagesData) continue;
                
                const nrpages = new DataView(nrpagesData.buffer, nrpagesData.byteOffset, nrpagesData.byteLength).getBigUint64(0, true);

                // Debug suspicious values
                if (nrpages > BigInt(0x100000)) { // More than 1M pages
                    console.log(`    WARNING: Huge nrpages at inode 0x${inodeAddr.toString(16)}: ${nrpages} (0x${nrpages.toString(16)})`);
                    console.log(`      mappingAddr: 0x${mappingAddr.toString(16)}`);
                    console.log(`      nrpagesPA: 0x${nrpagesPA.toString(16)}`);
                }

                if (nrpages > BigInt(0)) {
                    // Get inode number
                    const inoPA = this.translateKernelVA(inodeAddr + BigInt(this.offsets['inode.i_ino']));
                    let ino = BigInt(0);
                    if (inoPA) {
                        const inoData = this.memory.readBytes(inoPA - 0x40000000, 8);
                        if (inoData) {
                            ino = new DataView(inoData.buffer, inoData.byteOffset, inoData.byteLength).getBigUint64(0, true);
                            // Debug: Check if this looks like a kernel address instead of an inode number
                            if (ino > BigInt('0xffff000000000000')) {
                                console.log(`    WARNING: Inode number looks like kernel VA: ${ino} (0x${ino.toString(16)})`);
                                console.log(`      Read from inode addr 0x${inodeAddr.toString(16)} + offset 0x${this.offsets['inode.i_ino'].toString(16)}`);
                                console.log(`      Translated to PA: 0x${inoPA.toString(16)}`);
                            }
                        }
                    }
                    
                    // Get file size
                    const sizePA = this.translateKernelVA(inodeAddr + BigInt(this.offsets['inode.i_size']));
                    let fileSize = BigInt(0);
                    if (sizePA) {
                        const sizeData = this.memory.readBytes(sizePA - 0x40000000, 8);
                        if (sizeData) {
                            fileSize = new DataView(sizeData.buffer, sizeData.byteOffset, sizeData.byteLength).getBigUint64(0, true);
                        }
                    }
                    
                    const cachedFile: CachedFile = {
                        inode: Number(ino),
                        size: Number(fileSize),
                        cachedPages: Number(nrpages),
                        cacheSize: Number(nrpages) * 4096,
                        filesystem: fs.id || fs.type
                    };
                    
                    result.cachedFiles.push(cachedFile);
                    fs.cachedPages += Number(nrpages);
                    result.totalCachedPages += Number(nrpages);
                }
            }

            if (inodes.length > 0) {
                console.log(`  Summary: ${activeInodeCount} active inodes, ${skippedInodeCount} unallocated/inactive inodes`);
            }

            result.filesystems.push(fs);
        }
        
        result.totalCacheSize = result.totalCachedPages * 4096;
        
        // Summary
        console.log(`\n=== Discovery Summary ===`);
        console.log(`Total cached pages: ${result.totalCachedPages}`);
        console.log(`Total cache size: ${(result.totalCacheSize / (1024 * 1024)).toFixed(2)} MB`);
        console.log(`Number of filesystems: ${result.filesystems.length}`);
        console.log(`Number of cached files: ${result.cachedFiles.length}`);
        
        // Show top cached files
        if (result.cachedFiles.length > 0) {
            console.log('\nTop 5 cached files by size:');
            const sorted = [...result.cachedFiles].sort((a, b) => b.cacheSize - a.cacheSize).slice(0, 5);
            for (const file of sorted) {
                console.log(`  Inode ${file.inode}: ${(file.cacheSize / 1024).toFixed(0)} KB (${file.cachedPages} pages)`);
            }
        }
        
        return result;
    }

    /**
     * Discover open files through process file tables
     * This is more reliable than walking s_inodes which contains unallocated entries
     */
    async discoverOpenFiles(providedProcesses?: any[]): Promise<Map<bigint, any>> {
        console.log('\n=== Discovering Open Files Through Process File Tables ===\n');

        const openFiles = new Map<bigint, any>(); // inode address -> file info

        // Use provided processes, get from kernelDiscovery, or discover them
        let processes = providedProcesses;
        if (!processes && this.kernelDiscovery && typeof (this.kernelDiscovery as any).processes !== 'undefined') {
            // Get from kernelDiscovery if it has already found them
            const processMap = (this.kernelDiscovery as any).processes;
            if (processMap && processMap.size > 0) {
                console.log('Using processes from kernelDiscovery...');
                processes = Array.from(processMap.values());
            }
        }
        if (!processes) {
            console.log('No processes provided, discovering them with library...');
            const processMap = this.kmem.findProcesses();
            processes = Array.from(processMap.values());
        }

        console.log(`Processing ${processes.length} processes`);

        const tasks: Array<{addr: bigint, pid: number, name: string}> = [];

        for (const proc of processes) {
            const taskAddr = BigInt(proc.taskStruct);
            const pid = proc.pid;
            const name = proc.name;  // ProcessInfo uses 'name'

            // Skip kernel threads
            if (!name || name.startsWith('[') || proc.isKernelThread) continue;

            // Process user tasks
            tasks.push({addr: taskAddr, pid, name});

            // Get files_struct pointer using kmem helper
            const filesPtr = this.kmem.readU64(taskAddr + BigInt(this.offsets['task_struct.files']));
            if (filesPtr && filesPtr > BigInt('0xffff000000000000')) {
                // Read fdtable pointer
                const fdtPtr = this.kmem.readU64(filesPtr + BigInt(this.offsets['files_struct.fdt']));
                if (fdtPtr && fdtPtr > BigInt('0xffff000000000000')) {
                    // Read max_fds and fd array pointer
                    const maxFds = this.kmem.readU32(fdtPtr + BigInt(this.offsets['fdtable.max_fds']));
                    const fdArrayPtr = this.kmem.readU64(fdtPtr + BigInt(this.offsets['fdtable.fd']));

                    if (maxFds && fdArrayPtr) {
                        // Check first few file descriptors
                        const checkFds = Math.min(maxFds, 20);
                        let foundFiles = 0;

                        for (let fd = 0; fd < checkFds; fd++) {
                            const filePtr = this.kmem.readU64(fdArrayPtr + BigInt(fd * 8));
                            if (filePtr && filePtr > BigInt('0xffff000000000000')) {
                                // Read inode pointer from file structure
                                const inodePtr = this.kmem.readU64(filePtr + BigInt(this.offsets['file.f_inode']));
                                if (inodePtr && inodePtr > BigInt('0xffff000000000000')) {
                                    foundFiles++;

                                    if (!openFiles.has(inodePtr)) {
                                        // Read inode details
                                        const ino = this.kmem.readU64(inodePtr + BigInt(this.offsets['inode.i_ino'])) || BigInt(0);
                                        const size = this.kmem.readU64(inodePtr + BigInt(this.offsets['inode.i_size'])) || BigInt(0);

                                        openFiles.set(inodePtr, {
                                            inodeAddr: inodePtr,
                                            ino: ino,
                                            size: size,
                                            processes: [{pid, name, fd}]
                                        });
                                    } else {
                                        // Add this process to existing inode entry
                                        openFiles.get(inodePtr).processes.push({pid, name, fd});
                                    }
                                }
                            }
                        }

                        if (foundFiles > 0) {
                            console.log(`Process ${name} (PID ${pid}): found ${foundFiles} open files`);
                        }
                    }
                }
            }
        }

        console.log(`\nFound ${tasks.length} user processes`);
        console.log(`Discovered ${openFiles.size} unique open files`);

        // Show some examples
        if (openFiles.size > 0) {
            console.log('\nExample open files:');
            let count = 0;
            for (const [inodeAddr, info] of openFiles) {
                if (count++ >= 5) break;
                console.log(`  Inode 0x${inodeAddr.toString(16)}: #${info.ino}, size=${info.size} bytes`);
                console.log(`    Opened by: ${info.processes.map((p: any) => `${p.name}[${p.pid}]:fd${p.fd}`).join(', ')}`);
            }
        }

        return openFiles;
    }
}
