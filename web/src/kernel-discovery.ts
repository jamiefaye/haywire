/**
 * Kernel Discovery for Haywire - TypeScript Implementation
 *
 * Discovers kernel structures from memory dump:
 * 1. Process list with names
 * 2. PTEs per process
 * 3. Memory sections per process
 * 4. Kernel PTEs
 * 5. Reverse mapping (physical page -> PIDs)
 */

import { VirtualAddress, VA } from './types/virtual-address';
import { PhysicalAddress, PA } from './types/physical-address';
import { PageTableEntry, PGDEntry, walkPageTable } from './types/page-table';

// ARM64 Pointer Authentication stripping
export function stripPAC(ptr: VirtualAddress): VirtualAddress {
    // DON'T strip kernel virtual addresses!
    // Addresses like 0xffff00001d12b700 are valid kernel VAs, not PAC

    // Real pointer authentication would have unusual bit patterns
    // in the upper bits, not the standard 0xffff kernel prefix

    // For now, return addresses as-is
    // Kernel VAs (0xffff...) need to be translated through page tables,
    // not stripped

    return ptr;
}

// Configuration constants
export const KernelConstants = {
    TASK_STRUCT_SIZE: 9088,
    PID_OFFSET: 0x750,
    COMM_OFFSET: 0x970,
    MM_OFFSET: 0x6d0,  // Fixed: was 0x658, should be 1744 decimal
    PGD_OFFSET_IN_MM: 0x68,  // Fixed: was 0x48, should be 104 decimal

    // Linked list offsets (Linux 6.x)
    TASKS_LIST_OFFSET: 0x7e0,  // task_struct.tasks list_head
    LIST_HEAD_SIZE: 16,  // sizeof(struct list_head) = 2 pointers

    PAGE_SIZE: 4096,
    PAGE_SHIFT: 12,
    PTE_ENTRIES: 512,

    // SLAB offsets for task_structs
    SLAB_OFFSETS: [0x0, 0x2380, 0x4700],  // 32KB SLAB positions

    // Additional offsets for page-straddling task_structs
    // When scanning individual pages, task_structs that straddle boundaries
    // appear at these offsets in the subsequent pages
    PAGE_STRADDLE_OFFSETS: [0x0, 0x380, 0x700],

    // VMA (Virtual Memory Area) offsets in vm_area_struct
    // These are for ARM64 Linux 6.x - may vary by kernel version
    VMA_VM_START: 0x0,      // Start address of VMA
    VMA_VM_END: 0x8,        // End address of VMA
    VMA_VM_NEXT: 0x10,      // Next VMA in linked list
    VMA_VM_PREV: 0x18,      // Previous VMA in linked list
    VMA_VM_FLAGS: 0x50,     // Permission flags
    VMA_VM_FILE: 0x90,      // File backing this VMA (if any)

    // mm_struct VMA-related offsets
    // Note: These offsets vary by kernel version and config!
    // For typical ARM64 Linux 6.x:
    MM_MMAP: 0x10,          // First VMA in linked list (after spinlock/flags)
    MM_START_CODE: 0x98,    // Start of code segment
    MM_END_CODE: 0xa0,      // End of code segment
    MM_START_DATA: 0xa8,    // Start of data segment
    MM_END_DATA: 0xb0,      // End of data segment
    MM_START_BRK: 0xb8,     // Start of heap
    MM_BRK: 0xc0,           // Current heap end
    MM_START_STACK: 0xc8,   // Start of stack

    // VMA flags (from include/linux/mm.h)
    VM_READ: 0x00000001,
    VM_WRITE: 0x00000002,
    VM_EXEC: 0x00000004,
    VM_SHARED: 0x00000008,
    VM_MAYREAD: 0x00000010,
    VM_MAYWRITE: 0x00000020,
    VM_MAYEXEC: 0x00000040,
    VM_GROWSDOWN: 0x00000100,  // Stack segment
    VM_GROWSUP: 0x00000200,

    // Known kernel addresses
    SWAPPER_PGD: PA(0x082c00000),

    // Valid bit patterns
    PTE_VALID_MASK: 0x3,
    PTE_VALID_BITS: 0x3,

    // Address ranges
    GUEST_RAM_START: PA(0x40000000),
    GUEST_RAM_END: PA(0x1C0000000),  // 7GB total (1GB start + 6GB size)

    // Physical address mask for ARM64 (bits [47:12])
    PA_MASK: 0x0000FFFFFFFFF000,

    KERNEL_VA_START: VA(0xffff000000000000n), // Kernel virtual address start
} as const;

// Known good process names to validate against
const KNOWN_PROCESSES = [
    'init', 'systemd', 'kthreadd', 'rcu_gp', 'migration',
    'ksoftirqd', 'kworker', 'kcompactd', 'khugepaged',
    'kswapd', 'kauditd', 'sshd', 'systemd-journal',
    'systemd-resolved', 'systemd-networkd', 'bash'
];

// Type definitions
export interface ProcessInfo {
    pid: number;
    name: string;
    taskStruct: PhysicalAddress;
    mmStruct: VirtualAddress;  // Kernel virtual address to mm_struct
    pgd: PhysicalAddress;
    isKernelThread: boolean;
    tasksNext: PhysicalAddress;  // Next task in linked list
    tasksPrev: PhysicalAddress;  // Previous task in linked list
    files?: VirtualAddress;     // Pointer to files_struct (optional, may be null for kernel threads)
    ptes: PTE[];
    sections: MemorySection[];
}

export interface PTE {
    va: VirtualAddress;  // Virtual address
    pa: PhysicalAddress;  // Physical address
    flags: bigint;
    r: boolean;          // Readable
    w: boolean;          // Writable
    x: boolean;          // Executable
}

export interface MemorySection {
    type: 'code' | 'data' | 'heap' | 'stack' | 'library' | 'kernel';
    startVa: VirtualAddress;
    endVa: VirtualAddress;
    startPa: PhysicalAddress;
    size: number;
    pages: number;
    flags: bigint;
}

export interface DiscoveryStats {
    totalProcesses: number;
    kernelThreads: number;
    userProcesses: number;
    totalPTEs: number;
    kernelPTEs: number;
    uniquePages: number;
    sharedPages: number;
    zeroPages: number;
}

export interface DiscoveryOutput {
    processes: ProcessInfo[];
    ptesByPid: Map<number, PTE[]>;
    sectionsByPid: Map<number, MemorySection[]>;
    kernelPtes: PTE[];
    pageToPids: Map<string, Set<number>>;  // Using string key for PA comparison
    stats: DiscoveryStats;
    swapperPgDir?: PhysicalAddress;  // Physical address of discovered swapper_pg_dir
    pageCollection?: any;  // Type will be PageCollection when imported
}

export class KernelDiscovery {
    private memory: ArrayBuffer;
    private view: DataView;
    private baseOffset: number; // Offset of this chunk in the full file
    private decoder = new TextDecoder('ascii');

    // Data structures
    private processes = new Map<number, ProcessInfo>();
    private kernelPtes: PTE[] = [];
    private pageToPids = new Map<string, Set<number>>();  // Using string key for PA comparison
    private zeroPages = new Set<string>();  // Using string for PA
    private discoveredSwapperPgDir = PA(0);  // Track discovered swapper_pg_dir

    // Statistics
    private stats: DiscoveryStats = {
        totalProcesses: 0,
        kernelThreads: 0,
        userProcesses: 0,
        totalPTEs: 0,
        kernelPTEs: 0,
        uniquePages: 0,
        sharedPages: 0,
        zeroPages: 0,
    };

    constructor(memory: ArrayBuffer, baseOffset: number = 0) {
        this.memory = memory;
        this.view = new DataView(memory);
        this.baseOffset = baseOffset; // Track where this chunk starts in the full file
    }

    /**
     * Read 32-bit unsigned integer
     */
    private readU32(offset: number): number | null {
        if (offset + 4 > this.memory.byteLength) {
            return null;
        }
        return this.view.getUint32(offset, true); // little-endian
    }

    /**
     * Read 64-bit unsigned integer
     */
    private readU64(offset: number): bigint | null {
        if (offset + 8 > this.memory.byteLength) {
            return null;
        }
        return this.view.getBigUint64(offset, true); // little-endian
    }

    /**
     * Read null-terminated string
     */
    private readString(offset: number, maxLen: number = 16): string | null {
        if (offset + maxLen > this.memory.byteLength) {
            return null;
        }

        const bytes = new Uint8Array(this.memory, offset, maxLen);
        const nullIdx = bytes.indexOf(0);

        if (nullIdx === 0 || nullIdx > 15) {
            return null;
        }

        const stringBytes = bytes.slice(0, nullIdx);
        const str = this.decoder.decode(stringBytes);

        // Check for printable ASCII
        if (!str || !/^[\x20-\x7E]+$/.test(str)) {
            return null;
        }

        return str;
    }

    /**
     * Check if page is all zeros (optimize with SIMD when available)
     */
    private isZeroPage(offset: number): boolean {
        if (offset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            return false;
        }

        // Check first 256 bytes as sample
        const sample = new Uint8Array(this.memory, offset, 256);
        return sample.every(b => b === 0);
    }

    /**
     * Check if offset contains a valid task_struct
     */
    private checkTaskStruct(offset: number): ProcessInfo | null {
        // Check PID
        const pid = this.readU32(offset + KernelConstants.PID_OFFSET);
        if (!pid || pid < 1 || pid > 32768) {
            return null;
        }

        // Check comm (process name)
        const name = this.readString(offset + KernelConstants.COMM_OFFSET);
        if (!name) {
            return null;
        }

        // Get mm_struct pointer
        const mmPtrRaw = this.readU64(offset + KernelConstants.MM_OFFSET);
        const mmPtr = mmPtrRaw ? VA(mmPtrRaw) : null;

        // Get PGD if mm_struct is valid
        // Kernel structures can be at ~5GB mark (0x13XXXXXXX), within our 6GB file
        let pgdPtr = PA(0);
        if (mmPtr) {
            // Calculate offset directly from file start
            const mmFileOffset = Number(mmPtr) - Number(KernelConstants.GUEST_RAM_START);

            // Only read if mm_struct is within our memory buffer
            if (mmFileOffset >= 0 && mmFileOffset + KernelConstants.PGD_OFFSET_IN_MM + 8 <= this.memory.byteLength) {
                const pgdValue = this.readU64(mmFileOffset + KernelConstants.PGD_OFFSET_IN_MM);
                if (pgdValue) {
                    // Mask to get physical address
                    pgdPtr = PA(pgdValue & BigInt(KernelConstants.PA_MASK));

                    // Log first few for debugging
                    if (this.processes.size < 5) {
                        console.log(`  PID ${pid} (${name}): mm_struct=${VirtualAddress.toHex(mmPtr)}, pgd_raw=0x${pgdValue.toString(16)}, pgd_pa=${PhysicalAddress.toHex(pgdPtr)}`);
                    }
                }
            } else {
                // mm_struct is outside our buffer
                if (this.processes.size < 5) {
                    console.log(`  PID ${pid} (${name}): mm_struct=${VirtualAddress.toHex(mmPtr)} is outside buffer (offset would be 0x${mmFileOffset.toString(16)}`);
                }
            }
        }

        return {
            pid,
            name,
            taskStruct: PA(offset + this.baseOffset + Number(KernelConstants.GUEST_RAM_START)), // Absolute physical address
            mmStruct: mmPtr || VA(0),
            pgd: pgdPtr,
            isKernelThread: mmPtr === null,
            tasksNext: PA(0),  // Not used in this simplified version
            tasksPrev: PA(0),  // Not used in this simplified version
            ptes: [],
            sections: [],
        };
    }

    /**
     * Find all processes by scanning for task_structs
     */
    private findProcesses(): void {
        console.log('Finding processes...');
        const knownFound: string[] = [];

        for (let pageStart = 0; pageStart < this.memory.byteLength; pageStart += KernelConstants.PAGE_SIZE) {
            if (pageStart % (100 * 1024 * 1024) === 0) {
                console.log(`  Scanning ${pageStart / (1024 * 1024)}MB... (${this.processes.size} processes, ${knownFound.length} known)`);
            }

            for (const slabOffset of KernelConstants.SLAB_OFFSETS) {
                const offset = pageStart + slabOffset;
                if (offset + KernelConstants.TASK_STRUCT_SIZE > this.memory.byteLength) {
                    continue;
                }

                const process = this.checkTaskStruct(offset);
                if (process) {
                    this.processes.set(process.pid, process);

                    // Track known processes
                    if (KNOWN_PROCESSES.some(known => process.name.includes(known))) {
                        knownFound.push(process.name);
                    }
                }
            }
        }

        this.stats.totalProcesses = this.processes.size;
        this.stats.kernelThreads = Array.from(this.processes.values())
            .filter(p => p.isKernelThread).length;
        this.stats.userProcesses = this.stats.totalProcesses - this.stats.kernelThreads;

        console.log(`Found ${this.processes.size} processes`);
        console.log(`Known processes found: ${knownFound.slice(0, 10).join(', ')}`);
    }

    /**
     * Walk page tables to find PTEs
     */
    private walkPageTable(tableAddr: PhysicalAddress, level: number = 0): PTE[] {
        const ptes: PTE[] = [];

        // Page tables are at physical addresses around 5GB mark (0x13XXXXXXX)
        // They ARE in the memory-backend-file but might be beyond traditional "guest RAM"

        // Calculate offset from start of memory file
        const fileOffset = Number(tableAddr) - Number(KernelConstants.GUEST_RAM_START);

        // Check if this is within our memory buffer
        if (fileOffset < 0 || fileOffset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            // Page table not in our current view
            if (level === 0) {
                console.log(`    PGD at ${PhysicalAddress.toHex(tableAddr)} (file offset 0x${fileOffset.toString(16)}) is outside buffer range (0-0x${this.memory.byteLength.toString(16)}`);
            }
            return ptes;
        }

        for (let i = 0; i < KernelConstants.PTE_ENTRIES; i++) {
            const entryOffset = fileOffset + i * 8;
            const entry = this.readU64(entryOffset);
            if (!entry) continue;

            // Check if valid (bits [1:0] should be 0x3 for table or 0x1 for block)
            const entryType = Number(entry) & 0x3;
            if (entryType === 0) continue; // Invalid entry

            // Extract physical address using proper mask (bits [47:12])
            const physAddr = PA(entry & BigInt(KernelConstants.PA_MASK));

            if (level === 3) { // PTE level
                const virtualAddr = VA(BigInt(i) << BigInt(KernelConstants.PAGE_SHIFT));
                const flags = entry & 0xFFFn;

                ptes.push({
                    va: virtualAddr,
                    pa: physAddr,
                    flags,
                    r: (flags & 0x1n) !== 0n,
                    w: (flags & 0x2n) !== 0n,
                    x: (flags & 0x4n) === 0n, // NX bit inverted
                });
            } else if (entryType === 0x3 && level < 3) {
                // Table descriptor - recurse to next level
                const nextPtes = this.walkPageTable(physAddr, level + 1);
                ptes.push(...nextPtes);
            } else if (entryType === 0x1) {
                // Block descriptor - huge page
                // TODO: Handle huge pages properly based on level
            }
        }

        return ptes;
    }

    /**
     * Find PTEs for each process
     */
    private findProcessPTEs(): void {
        console.log('Finding PTEs for processes...');
        let processedCount = 0;
        let pteCount = 0;
        let noPgdCount = 0;
        let kernelThreadCount = 0;

        for (const process of this.processes.values()) {
            if (process.isKernelThread) {
                kernelThreadCount++;
                continue;  // Skip kernel threads, they don't have user PTEs
            }

            if (process.pgd && process.pgd !== PA(0)) {
                console.log(`  Walking page table for PID ${process.pid} (${process.name}), PGD: ${PhysicalAddress.toHex(process.pgd)}`);
                process.ptes = this.walkPageTable(process.pgd);

                if (process.ptes.length > 0) {
                    console.log(`    Found ${process.ptes.length} PTEs`);
                    pteCount += process.ptes.length;
                } else {
                    console.log(`    No PTEs found (PGD might be outside accessible range)`);
                }

                // Build reverse mapping
                for (const pte of process.ptes) {
                    if (pte.pa && pte.pa !== PA(0)) {
                        const paKey = PhysicalAddress.toHex(pte.pa);
                        if (!this.pageToPids.has(paKey)) {
                            this.pageToPids.set(paKey, new Set());
                        }
                        this.pageToPids.get(paKey)!.add(process.pid);
                    }
                }

                processedCount++;
            } else {
                noPgdCount++;
            }
        }

        console.log(`  Processed: ${processedCount} processes with PGDs`);
        console.log(`  Skipped: ${kernelThreadCount} kernel threads, ${noPgdCount} user processes without PGDs`);
        console.log(`  Total PTEs found: ${pteCount}`);

        this.stats.totalPTEs = Array.from(this.processes.values())
            .reduce((sum, p) => sum + p.ptes.length, 0);

        console.log(`Found ${this.stats.totalPTEs} total PTEs`);
    }

    /**
     * Check if offset contains a valid PTE table
     */
    private checkPteTable(offset: number): boolean {
        let validEntries = 0;
        let consecutiveValid = 0;
        let maxConsecutive = 0;

        for (let i = 0; i < KernelConstants.PTE_ENTRIES; i++) {
            const entry = this.readU64(offset + i * 8);
            if (!entry) {
                consecutiveValid = 0;
                continue;
            }

            // Check valid bit
            if ((Number(entry) & KernelConstants.PTE_VALID_BITS) === KernelConstants.PTE_VALID_BITS) {
                validEntries++;
                consecutiveValid++;
                maxConsecutive = Math.max(maxConsecutive, consecutiveValid);

                // Check if physical address is reasonable
                const physAddr = PA((entry >> 12n) << 12n);
                if (Number(physAddr) > Number(KernelConstants.GUEST_RAM_END)) { // Beyond guest RAM
                    return false;
                }
            } else {
                consecutiveValid = 0;
            }
        }

        // Need at least 50 valid entries and some consecutive ones
        return validEntries >= 50 && maxConsecutive >= 8;
    }

    /**
     * Find page tables directly by scanning memory
     */
    private findPageTables(): void {
        console.log('Finding page tables...');
        let pteTableCount = 0;

        // Key regions where page tables are typically found (scaled to chunk)
        const scanRegions = [
            [40 * 1024 * 1024, 100 * 1024 * 1024],     // 40-100MB
            [320 * 1024 * 1024, 360 * 1024 * 1024],    // 320-360MB
            [770 * 1024 * 1024, 930 * 1024 * 1024],    // 770-930MB
            [3590 * 1024 * 1024, 4100 * 1024 * 1024],  // 3.5-4.1GB
        ];

        for (const [regionStart, regionEnd] of scanRegions) {
            // Adjust for chunk offset
            const start = Math.max(0, regionStart - this.baseOffset);
            const end = Math.min(this.memory.byteLength, regionEnd - this.baseOffset);

            if (start >= end) {
                console.log(`  Skipping ${(regionStart / (1024*1024))}-${(regionEnd / (1024*1024))}MB (outside chunk)`);
                continue;
            }

            console.log(`  Scanning ${(regionStart / (1024*1024))}-${(regionEnd / (1024*1024))}MB (chunk offset: ${this.baseOffset / (1024*1024)}MB)...`);
            let pageCount = 0;

            for (let offset = start; offset < end - KernelConstants.PAGE_SIZE; offset += KernelConstants.PAGE_SIZE) {
                pageCount++;
                if (this.checkPteTable(offset)) {
                    pteTableCount++;
                    const absoluteAddr = this.baseOffset + offset + Number(KernelConstants.GUEST_RAM_START);
                    console.log(`    Found PTE table at offset 0x${(this.baseOffset + offset).toString(16)} (addr: 0x${absoluteAddr.toString(16)})`);

                    // Add a representative PTE entry for this table
                    this.kernelPtes.push({
                        va: VA(BigInt(absoluteAddr)),
                        pa: PA(BigInt(absoluteAddr)),
                        flags: BigInt(0x3), // Valid flags
                        r: true,
                        w: true,
                        x: false
                    });
                }
            }
            if (pageCount > 0) {
                console.log(`    Scanned ${pageCount} pages in this region`);
            }
        }

        console.log(`Found ${pteTableCount} page tables total`);
        this.stats.kernelPTEs = pteTableCount;
    }

    /**
     * Find swapper_pg_dir by signature scanning
     * Returns the physical address of swapper_pg_dir or 0 if not found
     */
    private findSwapperPgdBySignature(): number {
        console.log('Searching for swapper_pg_dir by adaptive signature...');

        // Detect estimated RAM size from file
        const totalFileSize = this.memory.byteLength + this.baseOffset;
        const estimatedRamSize = Math.ceil((totalFileSize - Number(KernelConstants.GUEST_RAM_START)) / (1024*1024*1024));
        console.log(`  Estimated VM RAM size: ~${estimatedRamSize}GB`);

        // Scan regions - broader to catch different configurations
        const scanRegions = [
            [0x70000000, 0x80000000],   // 1.75-2GB
            [0x80000000, 0x140000000],  // 2-5GB range
            [0x130000000, 0x140000000], // 4.75-5GB range (includes known ground truth)
        ];

        interface SwapperCandidate {
            physAddr: number;
            score: number;
            reasons: string[];
            pgd0PudCount: number;
            memSizeEstimate: number;
            userEntries: number;
            kernelEntries: number[];
        }

        const candidates: SwapperCandidate[] = [];

        for (const [regionStart, regionEnd] of scanRegions) {
            const start = Math.max(0, regionStart - Number(KernelConstants.GUEST_RAM_START) - this.baseOffset);
            const end = Math.min(this.memory.byteLength, regionEnd - Number(KernelConstants.GUEST_RAM_START) - this.baseOffset);
            if (start >= end) continue;

            // Quick pre-scan for sparse pages
            for (let offset = start; offset < end; offset += KernelConstants.PAGE_SIZE) {
                // Quick check for sparse page
                let nonZeroEntries = 0;
                for (let i = 0; i < 512; i++) {
                    const entry = this.readU64(offset + i * 8);
                    if (entry !== null && entry !== 0n) {
                        nonZeroEntries++;
                        if (nonZeroEntries > 20) break; // Too many entries
                    }
                }

                // Only analyze sparse pages (2-20 entries)
                if (nonZeroEntries >= 2 && nonZeroEntries <= 20) {
                    const physAddr = offset + this.baseOffset + Number(KernelConstants.GUEST_RAM_START);
                    const candidate = this.analyzeSwapperCandidate(offset, physAddr);
                    if (candidate && candidate.score >= 3) {
                        candidates.push(candidate);
                    }
                }
            }
        }

        // Sort by score
        candidates.sort((a, b) => b.score - a.score);

        console.log(`  Found ${candidates.length} high-scoring candidates`);

        if (candidates.length > 0) {
            // Show top candidates
            const top = candidates.slice(0, 5);
            for (const c of top) {
                console.log(`    PA: 0x${c.physAddr.toString(16)}, Score: ${c.score}, RAM: ${c.memSizeEstimate}GB, User: ${c.userEntries}, Kernel: ${c.kernelEntries.length}`);
            }

            const best = candidates[0];
            console.log(`  ✓ Best candidate at PA 0x${best.physAddr.toString(16)} (score: ${best.score}/7, est. RAM: ${best.memSizeEstimate}GB)`);
            return best.physAddr;
        }

        console.log('  swapper_pg_dir not found by signature scan');
        return 0;
    }

    /**
     * Check if a page matches the swapper_pg_dir signature
     * Signature: Mostly empty PGD with only 1-4 valid entries
     */
    private checkSwapperPgdSignature(offset: number): boolean {
        // Legacy method - kept for compatibility
        // The new approach uses sparse page detection in findSwapperPgdBySignature
        if (offset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            return false;
        }

        // Quick check for sparse page
        let nonZeroEntries = 0;
        for (let i = 0; i < 512; i++) {
            const entry = this.readU64(offset + i * 8);
            if (entry !== null && entry !== 0n) {
                nonZeroEntries++;
                if (nonZeroEntries > 20) return false;
            }
        }
        return nonZeroEntries >= 2 && nonZeroEntries <= 20;
    }

    /**
     * Validate potential swapper_pg_dir by checking PGD[0]'s PUD table
     * The PUD table should have many valid entries (not just 4)
     */
    private validateSwapperPgd(pgdOffset: number): boolean {
        // Legacy validation method - kept for compatibility
        // The new analyzeSwapperCandidate method includes validation with scoring
        const physAddr = pgdOffset + this.baseOffset + Number(KernelConstants.GUEST_RAM_START);
        const analysis = this.analyzeSwapperCandidate(pgdOffset, physAddr);
        return analysis !== null && analysis.score >= 3;
    }

    /**
     * Analyze a potential swapper_pg_dir candidate with adaptive scoring
     * Returns analysis object with score, reasons, and estimated memory size
     */
    private analyzeSwapperCandidate(offset: number, physAddr: number): any {
        if (offset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            return null;
        }

        const analysis = {
            physAddr,
            score: 0,
            reasons: [] as string[],
            pgd0PudCount: 0,
            memSizeEstimate: 0,
            userEntries: 0,
            kernelEntries: [] as number[],
        };

        // Count valid entries
        let pgd0Entry: { type: string; pa: number } | null = null;

        for (let i = 0; i < 512; i++) {
            const entry = this.readU64(offset + i * 8);
            if (!entry) continue;

            const entryType = Number(entry & 0x3n);

            if (entryType === 0x1 || entryType === 0x3) {
                if (i === 0) {
                    pgd0Entry = {
                        type: entryType === 0x1 ? 'BLOCK' : 'TABLE',
                        pa: Number(entry & BigInt(KernelConstants.PA_MASK))
                    };
                }
                if (i < 256) {
                    analysis.userEntries++;
                } else {
                    analysis.kernelEntries.push(i);
                }
            }
        }

        // Must have PGD[0] and sparse structure
        if (!pgd0Entry) return null;
        const totalEntries = analysis.userEntries + analysis.kernelEntries.length;
        if (totalEntries < 2 || totalEntries > 20) return null;

        // Check PUD count if PGD[0] is a TABLE
        if (pgd0Entry.type === 'TABLE') {
            const pudOffset = pgd0Entry.pa - Number(KernelConstants.GUEST_RAM_START) - this.baseOffset;
            if (pudOffset >= 0 && pudOffset + KernelConstants.PAGE_SIZE <= this.memory.byteLength) {
                let pudCount = 0;
                let consecutivePuds = true;

                for (let j = 0; j < 512; j++) {
                    const pud = this.readU64(pudOffset + j * 8);
                    if (pud !== null) {
                        const pudType = Number(pud & 0x3n);
                        if (pudType >= 1) {
                            pudCount++;
                            // Check if PUDs are NOT consecutive from index 0
                            if (pudCount > 1 && j !== pudCount - 1) {
                                consecutivePuds = false;
                            }
                        }
                    }
                }

                analysis.pgd0PudCount = pudCount;

                // Estimate memory size based on PUD count
                if (pudCount > 0 && consecutivePuds) {
                    analysis.memSizeEstimate = pudCount; // GB
                    analysis.score += 2;
                    analysis.reasons.push(`Linear mapping for ${pudCount}GB RAM`);

                    // Common RAM sizes get bonus points
                    if ([1, 2, 4, 6, 8, 16, 32].includes(pudCount)) {
                        analysis.score += 1;
                        analysis.reasons.push('Common RAM size');
                    }
                }
            }
        } else if (pgd0Entry.type === 'BLOCK') {
            // 1GB huge page for small memory systems
            analysis.memSizeEstimate = 1;
            analysis.score += 1;
            analysis.reasons.push('1GB block mapping');
        }

        // Check kernel entry patterns
        const hasKernelStart = analysis.kernelEntries.includes(256);
        const hasHighKernel = analysis.kernelEntries.some(idx => idx >= 500);

        if (hasKernelStart) {
            analysis.score += 1;
            analysis.reasons.push('Has kernel text mapping (PGD[256])');
        }

        if (hasHighKernel) {
            analysis.score += 1;
            analysis.reasons.push(`Has high kernel mappings`);
        }

        // Prefer single user entry (just PGD[0])
        if (analysis.userEntries === 1) {
            analysis.score += 1;
            analysis.reasons.push('Single user entry (expected)');
        }

        // Multiple kernel entries indicate complete kernel mapping
        if (analysis.kernelEntries.length >= 2) {
            analysis.score += 1;
            analysis.reasons.push(`${analysis.kernelEntries.length} kernel entries`);
        }

        return analysis;
    }

    /**
     * Try to get swapper_pg_dir from QMP (QEMU Machine Protocol)
     * Returns 0 if not available (browser mode, snapshot, or connection failure)
     */
    private async getSwapperPgdFromQMP(): Promise<number> {
        // Check if we have QMP access (Electron mode with running VM)
        // This is a placeholder - actual implementation would need to check
        // if we're in Electron and have access to QMP
        try {
            // In browser mode or with snapshots, this will fail
            // The actual implementation would use the QemuConnection class
            // to send query-kernel-info command
            console.log('  Attempting to get swapper_pg_dir from QMP...');

            // For now, return 0 to indicate QMP not available
            // Real implementation would do:
            // const kernelInfo = await this.qemuConnection.queryKernelInfo();
            // return Number(BigInt(kernelInfo.ttbr1) & BigInt(KernelConstants.PA_MASK));

            return 0;
        } catch (error) {
            console.log('  QMP not available (browser mode or snapshot)');
            return 0;
        }
    }

    /**
     * Find kernel PTEs from swapper_pg_dir
     */
    private async findKernelPTEs(): Promise<void> {
        console.log('Finding kernel PTEs...');

        // First try direct page table scanning
        this.findPageTables();

        let swapperPgd = 0;
        let groundTruthAvailable = false;

        // Try to get ground truth from QMP first
        const qmpPgd = await this.getSwapperPgdFromQMP();
        if (qmpPgd !== 0) {
            console.log(`  Ground truth from QMP: swapper_pg_dir at PA 0x${qmpPgd.toString(16)}`);
            swapperPgd = qmpPgd;
            groundTruthAvailable = true;
        }

        // Always run signature search to either confirm or discover
        const discoveredPgd = this.findSwapperPgdBySignature();

        if (groundTruthAvailable && discoveredPgd) {
            // We have both - check if they match
            if (discoveredPgd === swapperPgd) {
                console.log(`  ✅ Signature search confirmed QMP ground truth!`);
            } else {
                console.log(`  ⚠️ Signature search found different candidate: 0x${discoveredPgd.toString(16)}`);
                console.log(`     Using QMP ground truth as authoritative`);
            }
        } else if (!groundTruthAvailable && discoveredPgd) {
            // No QMP, but signature search found something
            console.log(`  Using signature-discovered swapper_pg_dir at PA 0x${discoveredPgd.toString(16)}`);
            swapperPgd = discoveredPgd;
        } else if (!groundTruthAvailable && !discoveredPgd) {
            // Neither worked - fall back to hardcoded value as last resort
            console.log(`  No QMP and signature search failed, trying hardcoded value...`);
            swapperPgd = Number(KernelConstants.SWAPPER_PGD);
        }

        // Store discovered value for output
        this.discoveredSwapperPgDir = PA(swapperPgd);

        // Check if the PGD is in our current memory chunk
        const absoluteOffset = swapperPgd - Number(KernelConstants.GUEST_RAM_START);
        const kernelPgdOffset = absoluteOffset - this.baseOffset;

        if (kernelPgdOffset < 0 || kernelPgdOffset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            console.log(`  swapper_pg_dir at ${PhysicalAddress.toHex(PA(swapperPgd))} is outside this chunk`);
            return;
        }

        console.log(`  Using swapper_pg_dir at PA ${PhysicalAddress.toHex(PA(swapperPgd))}`);
        const finalPgdOffset = kernelPgdOffset;

        // Walk kernel space entries (256-511)
        for (let i = 256; i < 512; i++) {
            const entryOffset = finalPgdOffset + i * 8;
            const entry = this.readU64(entryOffset);
            if (!entry || (Number(entry) & KernelConstants.PTE_VALID_MASK) !== KernelConstants.PTE_VALID_BITS) {
                continue;
            }

            // Follow to next level - extract physical address properly
            const nextTable = PA(entry & BigInt(KernelConstants.PA_MASK));
            const kernelPtes = this.walkPageTable(nextTable, 1);

            // Adjust virtual addresses for kernel space
            for (const pte of kernelPtes) {
                pte.va = VA(KernelConstants.KERNEL_VA_START | BigInt(i << 39) | pte.va);
                this.kernelPtes.push(pte);

                // Add to reverse mapping (PID 0 = kernel)
                if (pte.pa && pte.pa !== PA(0)) {
                    const paKey = PhysicalAddress.toHex(pte.pa);
                    if (!this.pageToPids.has(paKey)) {
                        this.pageToPids.set(paKey, new Set());
                    }
                    this.pageToPids.get(paKey)!.add(0);
                }
            }
        }

        if (this.kernelPtes.length > 0) {
            this.stats.kernelPTEs = this.kernelPtes.length;
            console.log(`Found ${this.kernelPtes.length} kernel PTEs from swapper_pg_dir`);
        }
    }

    /**
     * Extract memory sections (contiguous pages with same flags)
     */
    private extractSections(): void {
        console.log('Extracting memory sections...');

        for (const process of this.processes.values()) {
            if (process.ptes.length === 0) continue;

            const sections: MemorySection[] = [];
            let currentSection: MemorySection | null = null;

            // Sort PTEs by virtual address
            const sortedPtes = [...process.ptes].sort((a, b) =>
                VirtualAddress.compare(a.va, b.va));

            for (const pte of sortedPtes) {
                const va = pte.va;

                if (!currentSection) {
                    currentSection = {
                        type: this.determineType(va),
                        startVa: va,
                        endVa: VirtualAddress.add(va, KernelConstants.PAGE_SIZE),
                        startPa: pte.pa,
                        size: KernelConstants.PAGE_SIZE,
                        pages: 1,
                        flags: pte.flags,
                    };
                } else if (va === currentSection.endVa && pte.flags === currentSection.flags) {
                    // Extend current section
                    currentSection.endVa = VirtualAddress.add(va, KernelConstants.PAGE_SIZE);
                    currentSection.size = Number(currentSection.endVa) - Number(currentSection.startVa);
                    currentSection.pages++;
                } else {
                    // Start new section
                    sections.push(currentSection);
                    currentSection = {
                        type: this.determineType(va),
                        startVa: va,
                        endVa: VirtualAddress.add(va, KernelConstants.PAGE_SIZE),
                        startPa: pte.pa,
                        size: KernelConstants.PAGE_SIZE,
                        pages: 1,
                        flags: pte.flags,
                    };
                }
            }

            if (currentSection) {
                sections.push(currentSection);
            }

            process.sections = sections;
        }
    }

    /**
     * Determine section type based on virtual address
     */
    private determineType(va: VirtualAddress): MemorySection['type'] {
        if (VirtualAddress.isKernel(va)) return 'kernel';
        const addr = Number(va);
        if (addr < 0x400000) return 'code';
        if (addr < 0x600000) return 'data';
        if (addr >= 0x7f0000000000) return 'library';
        if (addr >= 0x7fff00000000) return 'stack';
        return 'heap';
    }

    /**
     * Identify zero pages to exclude from display
     */
    private identifyZeroPages(): void {
        console.log('Identifying zero pages...');

        let sampleCount = 0;
        const pageAddrs = Array.from(this.pageToPids.keys()).slice(0, 1000);

        for (const pageAddrKey of pageAddrs) {
            // Parse the hex string back to address
            const pageAddr = PA(pageAddrKey);
            if (pageAddr < KernelConstants.GUEST_RAM_START) continue;

            const offset = Number(pageAddr - KernelConstants.GUEST_RAM_START);
            if (this.isZeroPage(offset)) {
                this.zeroPages.add(pageAddrKey);
                sampleCount++;
            }
        }

        this.stats.zeroPages = this.zeroPages.size;
        console.log(`Found ${this.zeroPages.size} zero pages in sample`);
    }

    /**
     * Calculate final statistics
     */
    private calculateStats(): void {
        this.stats.uniquePages = this.pageToPids.size;
        this.stats.sharedPages = Array.from(this.pageToPids.values())
            .filter(pids => pids.size > 1).length;
    }

    /**
     * Main discovery function
     */
    public async discover(): Promise<DiscoveryOutput> {
        console.time('Kernel Discovery');

        this.findProcesses();
        this.findProcessPTEs();
        await this.findKernelPTEs();  // Now async
        this.extractSections();
        this.identifyZeroPages();
        this.calculateStats();

        console.timeEnd('Kernel Discovery');

        // Filter out zero pages from page mapping
        const filteredPageToPids = new Map<string, Set<number>>();
        for (const [pageKey, pids] of this.pageToPids.entries()) {
            if (!this.zeroPages.has(pageKey)) {
                filteredPageToPids.set(pageKey, pids);
            }
        }

        return {
            processes: Array.from(this.processes.values()),
            ptesByPid: new Map(
                Array.from(this.processes.entries())
                    .map(([pid, proc]) => [pid, proc.ptes])
            ),
            sectionsByPid: new Map(
                Array.from(this.processes.entries())
                    .map(([pid, proc]) => [pid, proc.sections])
            ),
            kernelPtes: this.kernelPtes,
            pageToPids: filteredPageToPids,
            stats: this.stats,
            swapperPgDir: this.discoveredSwapperPgDir || undefined,
        };
    }
}

/**
 * Helper function to format output for display
 */
export function formatDiscoveryOutput(output: DiscoveryOutput): any {
    return {
        processes: output.processes.map(p => ({
            pid: p.pid,
            name: p.name,
            isKernelThread: p.isKernelThread,
            taskStruct: `0x${p.taskStruct.toString(16)}`,
            pgd: p.pgd ? `0x${p.pgd.toString(16)}` : null,
            pteCount: p.ptes.length,
            sectionCount: p.sections.length,
        })),

        ptesByPid: Object.fromEntries(
            Array.from(output.ptesByPid.entries())
                .map(([pid, ptes]) => [
                    pid,
                    ptes.slice(0, 10).map(pte => ({
                        va: `0x${pte.va.toString(16)}`,
                        pa: `0x${pte.pa.toString(16)}`,
                        rwx: `${pte.r ? 'r' : '-'}${pte.w ? 'w' : '-'}${pte.x ? 'x' : '-'}`,
                    }))
                ])
        ),

        sectionsByPid: Object.fromEntries(
            Array.from(output.sectionsByPid.entries())
                .map(([pid, sections]) => [
                    pid,
                    sections.map(s => ({
                        type: s.type,
                        range: `0x${s.startVa.toString(16)}-0x${s.endVa.toString(16)}`,
                        size: `${(s.size / 1024).toFixed(1)}KB`,
                        pages: s.pages,
                    }))
                ])
        ),

        pageToPids: Object.fromEntries(
            Array.from(output.pageToPids.entries())
                .slice(0, 50)
                .map(([page, pids]) => [
                    page, // page is already a string (hex key)
                    Array.from(pids),
                ])
        ),

        stats: output.stats,
    };
}