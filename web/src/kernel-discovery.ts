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

    // Known kernel addresses
    SWAPPER_PGD: 0x082c00000,

    // Valid bit patterns
    PTE_VALID_MASK: 0x3,
    PTE_VALID_BITS: 0x3,

    // Address ranges
    GUEST_RAM_START: 0x40000000,
    GUEST_RAM_END: 0x200000000,
    KERNEL_VA_START: 0xffff000000000000n, // BigInt for 64-bit
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
    taskStruct: number;
    mmStruct: number;
    pgd: number;
    isKernelThread: boolean;
    tasksNext: number;  // Next task in linked list
    tasksPrev: number;  // Previous task in linked list
    ptes: PTE[];
    sections: MemorySection[];
}

export interface PTE {
    va: number | bigint;  // Virtual address
    pa: number;           // Physical address
    flags: number;
    r: boolean;          // Readable
    w: boolean;          // Writable
    x: boolean;          // Executable
}

export interface MemorySection {
    type: 'code' | 'data' | 'heap' | 'stack' | 'library' | 'kernel';
    startVa: number | bigint;
    endVa: number | bigint;
    startPa: number;
    size: number;
    pages: number;
    flags: number;
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
    pageToPids: Map<number, Set<number>>;
    stats: DiscoveryStats;
}

export class KernelDiscovery {
    private memory: ArrayBuffer;
    private view: DataView;
    private baseOffset: number; // Offset of this chunk in the full file
    private decoder = new TextDecoder('ascii');

    // Data structures
    private processes = new Map<number, ProcessInfo>();
    private kernelPtes: PTE[] = [];
    private pageToPids = new Map<number, Set<number>>();
    private zeroPages = new Set<number>();

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
        const mmPtr = Number(this.readU64(offset + KernelConstants.MM_OFFSET) || 0n);

        // Get PGD if mm_struct is valid
        let pgdPtr = 0;
        if (mmPtr && mmPtr >= KernelConstants.GUEST_RAM_START && mmPtr < KernelConstants.GUEST_RAM_END) {
            const mmAbsoluteOffset = mmPtr - KernelConstants.GUEST_RAM_START;
            const mmRelativeOffset = mmAbsoluteOffset - this.baseOffset;

            // Only read if mm_struct is within our chunk
            if (mmRelativeOffset >= 0 && mmRelativeOffset + KernelConstants.PGD_OFFSET_IN_MM + 8 <= this.memory.byteLength) {
                pgdPtr = Number(this.readU64(mmRelativeOffset + KernelConstants.PGD_OFFSET_IN_MM) || 0n);
            }
        }

        return {
            pid,
            name,
            taskStruct: KernelConstants.GUEST_RAM_START + this.baseOffset + offset, // Adjust for chunk's position
            mmStruct: mmPtr,
            pgd: pgdPtr,
            isKernelThread: mmPtr === 0,
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
    private walkPageTable(tableAddr: number, level: number = 0): PTE[] {
        const ptes: PTE[] = [];

        if (tableAddr < KernelConstants.GUEST_RAM_START || tableAddr >= KernelConstants.GUEST_RAM_END) {
            return ptes;
        }

        // Adjust for chunk offset - tableAddr is absolute, but we need relative to this chunk
        const absoluteOffset = tableAddr - KernelConstants.GUEST_RAM_START;
        const relativeOffset = absoluteOffset - this.baseOffset;

        if (relativeOffset < 0 || relativeOffset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            return ptes; // This table is not in our chunk
        }

        for (let i = 0; i < KernelConstants.PTE_ENTRIES; i++) {
            const entryOffset = relativeOffset + i * 8;
            const entry = this.readU64(entryOffset);
            if (!entry) continue;

            // Check if valid
            if ((Number(entry) & KernelConstants.PTE_VALID_MASK) !== KernelConstants.PTE_VALID_BITS) {
                continue;
            }

            const physAddr = Number((entry >> BigInt(KernelConstants.PAGE_SHIFT)) << BigInt(KernelConstants.PAGE_SHIFT));

            if (level === 3) { // PTE level
                const virtualAddr = i << KernelConstants.PAGE_SHIFT;
                const flags = Number(entry) & 0xFFF;

                ptes.push({
                    va: virtualAddr,
                    pa: physAddr,
                    flags,
                    r: (flags & 0x1) !== 0,
                    w: (flags & 0x2) !== 0,
                    x: (flags & 0x4) === 0, // NX bit inverted
                });
            } else {
                // Recurse to next level
                const nextPtes = this.walkPageTable(physAddr, level + 1);
                ptes.push(...nextPtes);
            }
        }

        return ptes;
    }

    /**
     * Find PTEs for each process
     */
    private findProcessPTEs(): void {
        console.log('Finding PTEs for processes...');

        for (const process of this.processes.values()) {
            if (process.pgd && process.pgd !== 0) {
                process.ptes = this.walkPageTable(process.pgd);

                // Build reverse mapping
                for (const pte of process.ptes) {
                    if (pte.pa) {
                        if (!this.pageToPids.has(pte.pa)) {
                            this.pageToPids.set(pte.pa, new Set());
                        }
                        this.pageToPids.get(pte.pa)!.add(process.pid);
                    }
                }
            }
        }

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
                const physAddr = (Number(entry) >> 12) << 12;
                if (physAddr > 0x200000000) { // Beyond 8GB
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
                    const absoluteAddr = this.baseOffset + offset + KernelConstants.GUEST_RAM_START;
                    console.log(`    Found PTE table at offset 0x${(this.baseOffset + offset).toString(16)} (addr: 0x${absoluteAddr.toString(16)})`);

                    // Add a representative PTE entry for this table
                    this.kernelPtes.push({
                        va: BigInt(absoluteAddr),
                        pa: absoluteAddr,
                        flags: 0x3, // Valid flags
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
     * Find kernel PTEs from swapper_pg_dir
     */
    private findKernelPTEs(): void {
        console.log('Finding kernel PTEs...');

        // First try direct page table scanning
        this.findPageTables();

        // Check if swapper_pg_dir is in this chunk
        const absoluteOffset = KernelConstants.SWAPPER_PGD - KernelConstants.GUEST_RAM_START;
        const kernelPgdOffset = absoluteOffset - this.baseOffset;

        if (kernelPgdOffset < 0 || kernelPgdOffset + KernelConstants.PAGE_SIZE > this.memory.byteLength) {
            console.log(`  swapper_pg_dir not in this chunk (needs offset 0x${absoluteOffset.toString(16)}, chunk starts at 0x${this.baseOffset.toString(16)})`);
            return;
        }

        console.log(`  Found swapper_pg_dir in this chunk at relative offset 0x${kernelPgdOffset.toString(16)}`)

        // Walk kernel space entries (256-511)
        for (let i = 256; i < 512; i++) {
            const entryOffset = kernelPgdOffset + i * 8;
            const entry = this.readU64(entryOffset);
            if (!entry || (Number(entry) & KernelConstants.PTE_VALID_MASK) !== KernelConstants.PTE_VALID_BITS) {
                continue;
            }

            // Follow to next level
            const nextTable = Number((entry >> BigInt(KernelConstants.PAGE_SHIFT)) << BigInt(KernelConstants.PAGE_SHIFT));
            const kernelPtes = this.walkPageTable(nextTable, 1);

            // Adjust virtual addresses for kernel space
            for (const pte of kernelPtes) {
                pte.va = Number(KernelConstants.KERNEL_VA_START | BigInt(i << 39) | BigInt(pte.va));
                this.kernelPtes.push(pte);

                // Add to reverse mapping (PID 0 = kernel)
                if (pte.pa) {
                    if (!this.pageToPids.has(pte.pa)) {
                        this.pageToPids.set(pte.pa, new Set());
                    }
                    this.pageToPids.get(pte.pa)!.add(0);
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
                Number(a.va) - Number(b.va));

            for (const pte of sortedPtes) {
                const va = Number(pte.va);

                if (!currentSection) {
                    currentSection = {
                        type: this.determineType(va),
                        startVa: va,
                        endVa: va + KernelConstants.PAGE_SIZE,
                        startPa: pte.pa,
                        size: KernelConstants.PAGE_SIZE,
                        pages: 1,
                        flags: pte.flags,
                    };
                } else if (va === Number(currentSection.endVa) && pte.flags === currentSection.flags) {
                    // Extend current section
                    currentSection.endVa = va + KernelConstants.PAGE_SIZE;
                    currentSection.size = Number(currentSection.endVa) - Number(currentSection.startVa);
                    currentSection.pages++;
                } else {
                    // Start new section
                    sections.push(currentSection);
                    currentSection = {
                        type: this.determineType(va),
                        startVa: va,
                        endVa: va + KernelConstants.PAGE_SIZE,
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
    private determineType(va: number | bigint): MemorySection['type'] {
        const addr = Number(va);
        if (addr < 0x400000) return 'code';
        if (addr < 0x600000) return 'data';
        if (addr >= 0x7f0000000000) return 'library';
        if (addr >= 0x7fff00000000) return 'stack';
        if (addr >= Number(KernelConstants.KERNEL_VA_START)) return 'kernel';
        return 'heap';
    }

    /**
     * Identify zero pages to exclude from display
     */
    private identifyZeroPages(): void {
        console.log('Identifying zero pages...');

        let sampleCount = 0;
        const pageAddrs = Array.from(this.pageToPids.keys()).slice(0, 1000);

        for (const pageAddr of pageAddrs) {
            if (pageAddr < KernelConstants.GUEST_RAM_START) continue;

            const offset = pageAddr - KernelConstants.GUEST_RAM_START;
            if (this.isZeroPage(offset)) {
                this.zeroPages.add(pageAddr);
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
        this.findKernelPTEs();
        this.extractSections();
        this.identifyZeroPages();
        this.calculateStats();

        console.timeEnd('Kernel Discovery');

        // Filter out zero pages from page mapping
        const filteredPageToPids = new Map<number, Set<number>>();
        for (const [page, pids] of this.pageToPids.entries()) {
            if (!this.zeroPages.has(page)) {
                filteredPageToPids.set(page, pids);
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
                    `0x${page.toString(16)}`,
                    Array.from(pids),
                ])
        ),

        stats: output.stats,
    };
}