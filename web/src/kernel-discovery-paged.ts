/**
 * Kernel Discovery for Paged Memory
 * Works with PagedMemory to handle large files without array size limits
 */

import { PagedMemory } from './paged-memory';
import { KernelConstants, ProcessInfo, PTE, MemorySection, DiscoveryStats, DiscoveryOutput } from './kernel-discovery';

// Export the types we use
export type { ProcessInfo, PTE, MemorySection, DiscoveryStats, DiscoveryOutput };
export { KernelConstants };

const KNOWN_PROCESSES = [
    'init', 'systemd', 'kthreadd', 'rcu_gp', 'migration',
    'ksoftirqd', 'kworker', 'kcompactd', 'khugepaged',
    'kswapd', 'kauditd', 'sshd', 'systemd-journal',
    'systemd-resolved', 'systemd-networkd', 'bash'
];

export class PagedKernelDiscovery {
    private memory: PagedMemory;
    private decoder = new TextDecoder('ascii');
    private totalSize: number = 0;

    // Data structures
    private processes = new Map<number, ProcessInfo>();
    private kernelPtes: PTE[] = [];
    private pageToPids = new Map<number, Set<number>>();
    private zeroPages = new Set<number>();
    private swapperPgDir: number = 0;

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

    constructor(memory: PagedMemory) {
        this.memory = memory;
    }

    /**
     * Check if value looks like a kernel pointer
     */
    private isKernelPointer(value: bigint): boolean {
        return (value >> 48n) === 0xffffn;
    }

    /**
     * Check if string is mostly printable and looks like a process name
     */
    private isPrintableString(str: string | null): boolean {
        if (!str || str.length < 2 || str.length > 15) return false;

        // Count different character types
        let alphaNum = 0;
        let special = 0;
        let invalid = 0;

        for (const c of str) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                alphaNum++;
            } else if (c === '/' || c === '-' || c === '_' || c === ':' || c === '.' || c === '[' || c === ']') {
                special++;  // Common in process names like kworker/0:1H
            } else if (c < ' ' || c > '~') {
                invalid++;
            }
        }

        // Must have mostly alphanumeric characters
        return alphaNum >= 2 && invalid === 0 && (alphaNum + special) >= str.length * 0.8;
    }

    /**
     * Validate that this task is part of a valid linked list
     */
    private validateLinkedList(offset: number): boolean {
        const listOffset = offset + KernelConstants.TASKS_LIST_OFFSET;
        const nextPtr = this.memory.readU64(listOffset);
        const prevPtr = this.memory.readU64(listOffset + 8);

        if (!nextPtr || !prevPtr) {
            return false;
        }

        // Both should be kernel pointers
        if (!this.isKernelPointer(nextPtr) || !this.isKernelPointer(prevPtr)) {
            return false;
        }

        return true;
    }

    /**
     * Check if offset contains a valid task_struct
     */
    private checkTaskStruct(offset: number): ProcessInfo | null {
        // Check PID
        const pid = this.memory.readU32(offset + KernelConstants.PID_OFFSET);
        if (!pid || pid < 1 || pid > 32768) {
            return null;
        }

        // Check comm (process name)
        const name = this.memory.readString(offset + KernelConstants.COMM_OFFSET);
        if (!name) {
            return null;
        }

        // Get mm_struct pointer
        const mmPtr = Number(this.memory.readU64(offset + KernelConstants.MM_OFFSET) || 0n);

        // Additional validation - check for kernel pointers nearby
        let kernelPtrCount = 0;
        for (let checkOffset = offset; checkOffset < offset + 512; checkOffset += 8) {
            const val = this.memory.readU64(checkOffset);
            if (val && this.isKernelPointer(val)) {
                kernelPtrCount++;
                if (kernelPtrCount >= 3) break;
            }
        }

        if (kernelPtrCount < 3) {
            return null;
        }

        // Check linked list validation
        const hasValidList = this.validateLinkedList(offset);

        // Read list pointers
        const tasksNext = Number(this.memory.readU64(offset + KernelConstants.TASKS_LIST_OFFSET) || 0n);
        const tasksPrev = Number(this.memory.readU64(offset + KernelConstants.TASKS_LIST_OFFSET + 8) || 0n);

        // Bonus validation: check if it's a known process name
        const isKnown = KNOWN_PROCESSES.some(known => name.includes(known));
        const isKernel = mmPtr === 0;

        // Log successful process discovery (commented out to reduce noise)
        // if (name && pid > 0) {
        //     console.log(`Found process: PID ${pid}, name="${name}", mm=0x${mmPtr.toString(16)}, kernel=${isKernel}`);
        // }

        // Basic requirements
        if (!name || pid < 1 || pid > 32768) {
            return null;
        }

        // Check if name is valid
        if (!this.isPrintableString(name) && !isKnown) {
            return null;
        }

        // Need at least one of: known name, valid list, or many kernel pointers
        if (!isKnown && !hasValidList && kernelPtrCount < 5) {
            return null;
        }

        // Additional validation for non-kernel threads
        if (!isKernel && mmPtr !== 0) {
            // mm_struct should be a kernel pointer or 0
            if (mmPtr > 0 && mmPtr < 0x100000000) {
                // Too low to be a kernel address
                return null;
            }
        }

        // Get PGD if mm_struct is valid
        let pgdPtr = 0;
        if (mmPtr && mmPtr >= KernelConstants.GUEST_RAM_START && mmPtr < KernelConstants.GUEST_RAM_END) {
            const mmOffset = mmPtr - KernelConstants.GUEST_RAM_START;
            pgdPtr = Number(this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM) || 0n);
        }

        return {
            pid,
            name,
            taskStruct: KernelConstants.GUEST_RAM_START + offset,
            mmStruct: mmPtr,
            pgd: pgdPtr,
            isKernelThread: isKernel,
            tasksNext,
            tasksPrev,
            ptes: [],
            sections: [],
        };
    }

    /**
     * Check if offset contains a valid PTE table
     */
    private checkPteTable(offset: number): boolean {
        // Read the entire 4KB page at once
        const pageBuffer = this.memory.readBytes(offset, KernelConstants.PAGE_SIZE);
        if (!pageBuffer) {
            return false;
        }

        // Create a DataView for efficient reading
        const view = new DataView(pageBuffer.buffer, pageBuffer.byteOffset, pageBuffer.byteLength);

        let validEntries = 0;
        let consecutiveValid = 0;
        let maxConsecutive = 0;

        for (let i = 0; i < KernelConstants.PTE_ENTRIES; i++) {
            // Read 64-bit little-endian value from buffer
            const entryLow = view.getUint32(i * 8, true);
            const entryHigh = view.getUint32(i * 8 + 4, true);
            const entry = entryLow + (entryHigh * 0x100000000);

            if (entry === 0) {
                consecutiveValid = 0;
                continue;
            }

            // Check valid bit
            if ((entry & KernelConstants.PTE_VALID_BITS) === KernelConstants.PTE_VALID_BITS) {
                validEntries++;
                consecutiveValid++;
                maxConsecutive = Math.max(maxConsecutive, consecutiveValid);

                // Check if physical address is reasonable
                const physAddr = (entry >> 12) << 12;
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
     * Find all processes by scanning for task_structs
     */
    private findProcesses(totalSize: number): void {
        console.log('Finding processes...');
        const knownFound: string[] = [];

        for (let pageStart = 0; pageStart < totalSize; pageStart += KernelConstants.PAGE_SIZE) {
            if (pageStart % (100 * 1024 * 1024) === 0) {
                console.log(`  Scanning ${pageStart / (1024 * 1024)}MB... (${this.processes.size} processes, ${knownFound.length} known)`);
            }

            // Try both SLAB offsets (for 32KB aligned regions) and page-straddle offsets
            const offsetsToCheck = [
                ...KernelConstants.SLAB_OFFSETS,
                ...KernelConstants.PAGE_STRADDLE_OFFSETS
            ];

            for (const slabOffset of offsetsToCheck) {
                const offset = pageStart + slabOffset;
                if (offset + KernelConstants.TASK_STRUCT_SIZE > totalSize) {
                    continue;
                }

                const process = this.checkTaskStruct(offset);
                if (process && !this.processes.has(process.pid)) {
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
     * Find all swapper_pg_dir candidates
     */
    private findSwapperPgDirCandidates(): Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> {
        console.log('Finding swapper_pg_dir candidates...');
        const candidates: Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> = [];

        // Common locations for swapper_pg_dir (relative to GUEST_RAM_START)
        const potentialLocations = [
            0x082c00000 - KernelConstants.GUEST_RAM_START,  // Previously found location
            0x041000000 - KernelConstants.GUEST_RAM_START,  // Recent finding
            0x042c00000 - KernelConstants.GUEST_RAM_START,  // Alternative
            0x463d4000 - KernelConstants.GUEST_RAM_START,   // Latest finding
        ];

        // First check known locations
        for (const offset of potentialLocations) {
            if (offset < 0 || offset >= this.totalSize) {
                continue;
            }

            const candidate = this.evaluatePgdCandidate(offset);
            if (candidate) {
                candidates.push(candidate);
                // Only log if it's a good candidate
                if (candidate.score > 40) {
                    console.log(`  Known location candidate at 0x${candidate.addr.toString(16)}: score=${candidate.score}`);
                }
            }
        }

        // Scan broader regions for more candidates
        const scanRegions = [
            [0x02000000, 0x10000000],  // 32MB - 256MB
            [0x20000000, 0x30000000],  // 512MB - 768MB
            [0x40000000, 0x50000000],  // 1GB - 1.25GB region
            [0x60000000, 0x70000000],  // 1.5GB - 1.75GB
        ];

        for (const [start, end] of scanRegions) {
            if (start >= this.totalSize) continue;
            const scanEnd = Math.min(end, this.totalSize);

            // Check every 4KB (page aligned)
            for (let offset = start; offset < scanEnd; offset += 4096) {
                const candidate = this.evaluatePgdCandidate(offset);
                if (candidate) {
                    candidates.push(candidate);
                    // Only log very high-scoring candidates during scan
                    if (candidate.score > 70) {
                        console.log(`  High-score candidate at 0x${candidate.addr.toString(16)}: score=${candidate.score}`);
                    }
                }
            }
        }

        return candidates;
    }

    /**
     * Evaluate if an offset could be a PGD by chasing pointers
     */
    private evaluatePgdCandidate(offset: number): {addr: number, score: number, kernelEntries: number, userEntries: number} | null {
        let kernelEntries = 0;
        let userEntries = 0;
        let validChains = 0;  // Count of successfully chased pointer chains

        // Read the page
        const pageBuffer = this.memory.readBytes(offset, KernelConstants.PAGE_SIZE);
        if (!pageBuffer) return null;

        const view = new DataView(pageBuffer.buffer, pageBuffer.byteOffset, pageBuffer.byteLength);

        // Check a sample of entries by chasing their chains
        const entriesToTest = [0, 1, 256, 257, 258, 300, 400, 500]; // Mix of user and kernel

        for (const i of entriesToTest) {
            const entryLow = view.getUint32(i * 8, true);
            const entryHigh = view.getUint32(i * 8 + 4, true);
            const entry = entryLow + (entryHigh * 0x100000000);

            if ((entry & KernelConstants.PTE_VALID_BITS) !== KernelConstants.PTE_VALID_BITS) {
                continue;
            }

            // Count the entry
            if (i < 256) userEntries++;
            else kernelEntries++;

            // Try to chase the pointer chain
            const pudAddr = entry & ~0xFFF;
            if (pudAddr < KernelConstants.GUEST_RAM_START || pudAddr >= KernelConstants.GUEST_RAM_END) {
                continue;
            }

            // Read the PUD table and check if it looks valid
            const pudOffset = pudAddr - KernelConstants.GUEST_RAM_START;
            const pudData = this.memory.readBytes(pudOffset, 64); // Read first 8 entries
            if (!pudData) continue;

            const pudView = new DataView(pudData.buffer, pudData.byteOffset, pudData.byteLength);
            let validPudEntries = 0;

            for (let j = 0; j < 8; j++) {
                const pudEntryLow = pudView.getUint32(j * 8, true);
                const pudEntryHigh = pudView.getUint32(j * 8 + 4, true);
                const pudEntry = pudEntryLow + (pudEntryHigh * 0x100000000);

                if ((pudEntry & 3) === 3) {
                    const pmdAddr = pudEntry & ~0xFFF;
                    if (pmdAddr >= KernelConstants.GUEST_RAM_START && pmdAddr < KernelConstants.GUEST_RAM_END) {
                        validPudEntries++;
                    }
                }
            }

            if (validPudEntries > 0) {
                validChains++;
            }
        }

        // Need at least one valid chain to be a potential PGD
        if (validChains < 1) return null;

        // Count all kernel entries for scoring
        let totalKernelEntries = 0;
        let totalUserEntries = 0;
        for (let i = 0; i < 512; i++) {
            const entryLow = view.getUint32(i * 8, true);
            const entryHigh = view.getUint32(i * 8 + 4, true);
            const entry = entryLow + (entryHigh * 0x100000000);

            if ((entry & 3) === 3) {
                if (i < 256) totalUserEntries++;
                else totalKernelEntries++;
            }
        }

        // Score based on valid chains and entry distribution
        let score = validChains * 50;  // Heavy weight on valid chains
        if (totalKernelEntries >= 15) score += totalKernelEntries * 2;
        if (totalUserEntries <= 5) score += (5 - totalUserEntries) * 5;

        return {
            addr: offset + KernelConstants.GUEST_RAM_START,
            score,
            kernelEntries: totalKernelEntries,
            userEntries: totalUserEntries
        };
    }

    /**
     * Find best swapper_pg_dir from candidates
     */
    private findSwapperPgDir(): number {
        const candidates = this.findSwapperPgDirCandidates();

        if (candidates.length === 0) {
            console.log('No swapper_pg_dir candidates found');
            return 0;
        }

        // Sort by score
        candidates.sort((a, b) => b.score - a.score);

        console.log(`Found ${candidates.length} swapper_pg_dir candidates`);

        // Debug: Show what processes we're working with
        let processCount = 0;
        console.log('Sample processes with kernel mm_structs:');
        for (const process of this.processes.values()) {
            if (process.mmStruct && process.mmStruct > 0xffff000000000000) {
                console.log(`  ${process.name}: mm_struct=0x${process.mmStruct.toString(16)}`);
                processCount++;
                if (processCount >= 5) break;
            }
        }

        // First, specifically test our known good candidate
        const knownGood = 0x463d4000;
        console.log(`Testing known candidate: 0x${knownGood.toString(16)}`);
        if (this.validateSwapperPgDir(knownGood)) {
            console.log(`  ✓ VALIDATED - Known candidate works!`);
            return knownGood;
        } else {
            console.log(`  ✗ Known candidate failed validation`);
        }

        // Test top candidates with actual VA translation
        let validatedCandidate: number = 0;
        for (let i = 0; i < Math.min(10, candidates.length); i++) {
            const c = candidates[i];
            console.log(`Testing candidate #${i+1}: 0x${c.addr.toString(16)} (score: ${c.score})`);

            if (this.validateSwapperPgDir(c.addr)) {
                console.log(`  ✓ VALIDATED - This appears to be the correct swapper_pg_dir!`);
                validatedCandidate = c.addr;
                break;
            } else {
                console.log(`  ✗ Failed validation`);
            }
        }

        if (!validatedCandidate) {
            console.log('Warning: No candidates passed validation, using highest score');
            validatedCandidate = candidates[0].addr;
        }

        console.log(`Selected swapper_pg_dir: 0x${validatedCandidate.toString(16)}`);
        return validatedCandidate;
    }

    /**
     * Find page tables directly by scanning memory
     */
    private findPageTables(totalSize: number): void {
        console.log('Finding page tables...');
        let pteTableCount = 0;

        // Scan the entire memory space, with progress updates every 500MB
        const chunkSize = 500 * 1024 * 1024; // 500MB chunks for progress reporting

        for (let chunkStart = 0; chunkStart < totalSize; chunkStart += chunkSize) {
            const chunkEnd = Math.min(chunkStart + chunkSize, totalSize);

            console.log(`  Scanning ${(chunkStart / (1024*1024)).toFixed(0)}-${(chunkEnd / (1024*1024)).toFixed(0)}MB...`);
            let pageCount = 0;

            for (let offset = chunkStart; offset < chunkEnd - KernelConstants.PAGE_SIZE; offset += KernelConstants.PAGE_SIZE) {
                pageCount++;
                if (this.checkPteTable(offset)) {
                    pteTableCount++;
                    const absoluteAddr = offset + KernelConstants.GUEST_RAM_START;

                    // Add a representative PTE entry for this table
                    this.kernelPtes.push({
                        va: BigInt(absoluteAddr),
                        pa: absoluteAddr,
                        flags: 0x3,
                        r: true,
                        w: true,
                        x: false
                    });
                }
            }

            // Log progress every 500 PTEs found
            if (pteTableCount > 0 && pteTableCount % 500 === 0) {
                console.log(`    Found ${pteTableCount} page tables so far`);
            }
        }

        console.log(`Found ${pteTableCount} page tables total`);
        this.stats.kernelPTEs = pteTableCount;
    }

    /**
     * Translate a virtual address to physical address using page tables
     * @param va Virtual address to translate
     * @param pgdBase Physical address of PGD to use
     */
    private translateVA(va: bigint | number, pgdBase: number): number | null {
        const virtualAddr = typeof va === 'number' ? BigInt(va) : va;

        // ARM64 uses SEPARATE page tables:
        // - TTBR0_EL1: User space (0x0000...)
        // - TTBR1_EL1: Kernel space (0xffff...) - uses swapper_pg_dir
        // The pgdBase parameter should be the appropriate PGD for the VA type

        // Extract indices for 4-level page table walk
        const pgdIndex = Number((virtualAddr >> 39n) & 0x1FFn);  // bits 47-39

        const pudIndex = Number((virtualAddr >> 30n) & 0x1FFn);  // bits 38-30
        const pmdIndex = Number((virtualAddr >> 21n) & 0x1FFn);  // bits 29-21
        const pteIndex = Number((virtualAddr >> 12n) & 0x1FFn);  // bits 20-12
        const pageOffset = Number(virtualAddr & 0xFFFn);        // bits 11-0

        // Read PGD entry
        const pgdOffset = pgdBase - KernelConstants.GUEST_RAM_START + (pgdIndex * 8);
        const pgdEntry = this.memory.readU64(pgdOffset);
        if (!pgdEntry) return null;

        const pgdType = Number(pgdEntry) & 3;
        if (pgdType === 0) return null; // Invalid
        if (pgdType === 1) {
            // PGD block descriptor (rare, but possible)
            const pageBase = Number(pgdEntry) & ~0xFFF;
            return pageBase + Number(virtualAddr & 0xFFFn);
        }
        // pgdType === 3: Table descriptor, continue to PUD

        // Extract PUD table address
        const pudBase = Number(pgdEntry) & ~0xFFF;

        // Read PUD entry
        const pudOffset = pudBase - KernelConstants.GUEST_RAM_START + (pudIndex * 8);
        const pudEntry = this.memory.readU64(pudOffset);
        if (!pudEntry) return null;

        const pudType = Number(pudEntry) & 3;
        if (pudType === 0) return null; // Invalid
        if (pudType === 1) {
            // 1GB block at PUD level
            const pageBase = Number(pudEntry) & ~0x3FFFFFFF;
            return pageBase + Number(virtualAddr & 0x3FFFFFFFn);
        }
        // pudType === 3: Table descriptor, continue to PMD

        // Extract PMD table address
        const pmdBase = Number(pudEntry) & ~0xFFF;

        // Read PMD entry
        const pmdOffset = pmdBase - KernelConstants.GUEST_RAM_START + (pmdIndex * 8);
        const pmdEntry = this.memory.readU64(pmdOffset);
        if (!pmdEntry) return null;

        const pmdType = Number(pmdEntry) & 3;
        if (pmdType === 0) return null; // Invalid
        if (pmdType === 1) {
            // 2MB block at PMD level
            const pageBase = Number(pmdEntry) & ~0x1FFFFF;
            return pageBase + Number(virtualAddr & 0x1FFFFFn);
        }
        // pmdType === 3: Table descriptor, continue to PTE

        // Extract PTE table address
        const pteBase = Number(pmdEntry) & ~0xFFF;

        // Read PTE entry
        const pteOffset = pteBase - KernelConstants.GUEST_RAM_START + (pteIndex * 8);
        const pteEntry = this.memory.readU64(pteOffset);
        if (!pteEntry || (Number(pteEntry) & 3) !== 3) {
            return null; // Invalid or not present
        }

        // Extract physical page address and add offset
        const pageBase = Number(pteEntry) & ~0xFFF;
        return pageBase + pageOffset;
    }

    /**
     * Validate a swapper_pg_dir candidate by testing the full chain
     */
    private validateSwapperPgDir(pgdAddr: number): boolean {
        let successCount = 0;
        let failCount = 0;
        let debugFirst = true;

        // Test with known kernel VAs from processes
        for (const process of this.processes.values()) {
            if (process.mmStruct && process.mmStruct > 0xffff000000000000) {
                // Step 1: Use candidate PGD to translate mm_struct VA→PA
                const mmPa = this.translateVA(BigInt(process.mmStruct), pgdAddr);

                if (debugFirst) {
                    console.log(`    Testing ${process.name}:`);
                    console.log(`      mm_struct VA: 0x${process.mmStruct.toString(16)}`);
                    if (mmPa) {
                        console.log(`      → mm_struct PA: 0x${mmPa.toString(16)}`);
                    } else {
                        console.log(`      → Translation failed`);
                        this.debugTranslateVA(BigInt(process.mmStruct), pgdAddr);
                        debugFirst = false;
                        failCount++;
                        continue;
                    }
                }

                if (!mmPa || mmPa < KernelConstants.GUEST_RAM_START || mmPa >= KernelConstants.GUEST_RAM_END) {
                    failCount++;
                    continue;
                }

                // Step 2: Read PGD pointer from mm_struct
                const mmOffset = mmPa - KernelConstants.GUEST_RAM_START;
                const processPgdPtr = this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM);
                if (!processPgdPtr) {
                    failCount++;
                    continue;
                }

                const processPgd = Number(processPgdPtr);
                if (debugFirst) {
                    console.log(`      Process PGD from mm_struct: 0x${processPgd.toString(16)}`);
                }

                // Step 3: Validate the process PGD looks reasonable
                if (processPgd < KernelConstants.GUEST_RAM_START || processPgd >= KernelConstants.GUEST_RAM_END) {
                    if (debugFirst) {
                        console.log(`      → Invalid PGD address`);
                    }
                    failCount++;
                    continue;
                }

                // Step 4: Ultimate test - use process PGD to translate its own mm_struct
                const verifyPa = this.translateVA(BigInt(process.mmStruct), processPgd);
                if (verifyPa === mmPa) {
                    if (debugFirst) {
                        console.log(`      ✓ Process PGD successfully translates its own mm_struct!`);
                    }
                    successCount++;
                    process.pgd = processPgd;
                } else {
                    if (debugFirst) {
                        console.log(`      ✗ Process PGD failed to translate correctly`);
                        console.log(`        Expected PA: 0x${mmPa.toString(16)}, got: ${verifyPa ? '0x' + verifyPa.toString(16) : 'null'}`);
                    }
                    failCount++;
                }

                debugFirst = false;

                // Test a few to get a sample
                if (successCount + failCount >= 5) break;
            }
        }

        if (successCount + failCount === 0) {
            console.log(`    No kernel VAs found to test`);
            return false;
        }

        const successRate = successCount / (successCount + failCount);
        console.log(`    Final: ${successCount}/${successCount + failCount} full chains validated (${(successRate * 100).toFixed(0)}%)`);
        return successRate > 0.6;  // Lower threshold since this is harder
    }

    /**
     * Debug version of translateVA to see where it fails
     */
    private debugTranslateVA(va: bigint, pgdBase: number): void {
        const virtualAddr = va;

        // Extract indices
        const pgdIndex = Number((virtualAddr >> 39n) & 0x1FFn);
        const pudIndex = Number((virtualAddr >> 30n) & 0x1FFn);
        const pmdIndex = Number((virtualAddr >> 21n) & 0x1FFn);
        const pteIndex = Number((virtualAddr >> 12n) & 0x1FFn);

        console.log(`      Indices: PGD[${pgdIndex}] PUD[${pudIndex}] PMD[${pmdIndex}] PTE[${pteIndex}]`);

        // Read PGD entry
        const pgdOffset = pgdBase - KernelConstants.GUEST_RAM_START + (pgdIndex * 8);
        const pgdEntry = this.memory.readU64(pgdOffset);
        console.log(`      PGD entry at 0x${(pgdBase + pgdIndex * 8).toString(16)}: ${pgdEntry ? '0x' + pgdEntry.toString(16) : 'null'}`);

        if (!pgdEntry || (Number(pgdEntry) & 3) !== 3) {
            console.log(`      → PGD entry invalid or not present`);
            return;
        }

        const pudBase = Number(pgdEntry) & ~0xFFF;
        const pudOffset = pudBase - KernelConstants.GUEST_RAM_START + (pudIndex * 8);
        const pudEntry = this.memory.readU64(pudOffset);
        console.log(`      PUD entry at 0x${(pudBase + pudIndex * 8).toString(16)}: ${pudEntry ? '0x' + pudEntry.toString(16) : 'null'}`);

        if (!pudEntry || (Number(pudEntry) & 3) !== 3) {
            console.log(`      → PUD entry invalid or not present`);
            return;
        }

        // Continue for PMD and PTE...
    }

    /**
     * Main discovery function
     */
    public async discover(totalSize: number): Promise<DiscoveryOutput> {
        console.time('Paged Kernel Discovery');
        console.log(`Starting discovery on ${(totalSize / (1024*1024)).toFixed(0)}MB of memory`);
        console.log(`Memory usage: ${this.memory.getMemoryUsage()}`);

        this.totalSize = totalSize;

        this.findProcesses(totalSize);
        this.swapperPgDir = this.findSwapperPgDir();

        // Run exhaustive PGD search
        console.log('\n=== STARTING EXHAUSTIVE PGD SEARCH ===');
        const allPGDs = this.findAllPGDs();
        console.log(`Found ${allPGDs.length} PGD candidates in full memory scan`);

        // Test top candidates to find the real kernel PGD
        console.log('\n=== TESTING TOP PGD CANDIDATES ===');
        let realKernelPGD = 0;
        for (let i = 0; i < Math.min(20, allPGDs.length); i++) {
            const candidate = allPGDs[i];
            console.log(`\nTesting candidate #${i+1}: 0x${candidate.addr.toString(16)} (score=${candidate.score})`);

            // KPTI means we have separate user/kernel PGDs
            // Test if this candidate can translate kernel VAs
            let successCount = 0;
            let testCount = 0;

            for (const process of this.processes.values()) {
                if (process.mmStruct && process.mmStruct > 0xffff000000000000n) {
                    const pa = this.translateVA(process.mmStruct, candidate.addr);
                    testCount++;

                    if (pa && pa >= KernelConstants.GUEST_RAM_START && pa < KernelConstants.GUEST_RAM_END) {
                        // Further validate: read the PA and check if it looks like mm_struct
                        const mmData = this.memory.readU64(pa - KernelConstants.GUEST_RAM_START + KernelConstants.PGD_OFFSET_IN_MM);
                        if (mmData) {
                            successCount++;
                            if (successCount === 1) {
                                console.log(`  ✓ Translated mm_struct 0x${process.mmStruct.toString(16)} to PA 0x${pa.toString(16)}`);
                            }
                        }
                    }

                    if (testCount >= 5) break; // Test a few
                }
            }

            if (successCount >= 3) {
                console.log(`  ✓✓✓ FOUND REAL KERNEL PGD! Successfully translated ${successCount}/${testCount} kernel VAs`);
                realKernelPGD = candidate.addr;
                this.swapperPgDir = candidate.addr;
                break;
            } else if (successCount > 0) {
                console.log(`  → Partial success: ${successCount}/${testCount} translations worked`);
            } else {
                console.log(`  → Failed to translate any kernel VAs`);
            }
        }

        if (realKernelPGD) {
            console.log(`\n=== SUCCESS: Real kernel PGD found at 0x${realKernelPGD.toString(16)} ===`);
        } else {
            console.log(`\n=== WARNING: No PGD candidate validated as real kernel PGD ===`);
        }

        this.findPageTables(totalSize);

        console.timeEnd('Paged Kernel Discovery');

        return {
            processes: Array.from(this.processes.values()),
            ptesByPid: new Map(),
            sectionsByPid: new Map(),
            kernelPtes: this.kernelPtes,
            pageToPids: new Map(),
            swapperPgDir: this.swapperPgDir,
            stats: this.stats,
        };
    }

    /**
     * Find ALL PGDs in the entire memory space (standalone, at bottom for easy finding)
     */
    public findAllPGDs(): Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> {
        console.log('=== EXHAUSTIVE PGD SEARCH ===');
        console.log(`Scanning entire ${(this.totalSize / (1024*1024)).toFixed(0)}MB memory space for PGDs...`);

        const pgdCandidates: Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> = [];
        let pagesScanned = 0;
        let candidatesFound = 0;

        // Scan EVERY 4KB page in the entire memory space
        for (let offset = 0; offset < this.totalSize; offset += 4096) {
            pagesScanned++;

            // Progress update every 1GB
            if (offset > 0 && offset % (1024 * 1024 * 1024) === 0) {
                console.log(`  Scanned ${(offset / (1024*1024*1024)).toFixed(1)}GB: found ${candidatesFound} PGD candidates so far...`);
            }

            // Quick pre-check: Read first few entries to see if it could be a PGD
            const pageBuffer = this.memory.readBytes(offset, 64); // First 8 entries
            if (!pageBuffer) continue;

            const view = new DataView(pageBuffer.buffer, pageBuffer.byteOffset, pageBuffer.byteLength);

            // Check if any entries look like page table pointers
            // Check both user space (0-7) and kernel space (256-263)
            let possiblePTE = false;
            for (let i = 0; i < 8; i++) {
                const entryLow = view.getUint32(i * 8, true);
                const entryHigh = view.getUint32(i * 8 + 4, true);
                const entry = entryLow + (entryHigh * 0x100000000);

                // Check if entry has valid bits (1 or 3)
                const entryType = entry & 3;
                if (entryType === 1 || entryType === 3) {
                    possiblePTE = true;
                    break;
                }
            }

            if (!possiblePTE) continue;

            // Full evaluation with chain validation
            const candidate = this.evaluatePgdCandidate(offset);
            if (candidate) {
                pgdCandidates.push(candidate);
                candidatesFound++;

                // Log first 20 candidates or high-scoring ones
                if (candidatesFound <= 20 || candidate.score >= 100) {
                    console.log(`  Found PGD at 0x${candidate.addr.toString(16)}: score=${candidate.score}, kernel=${candidate.kernelEntries}, user=${candidate.userEntries}`);
                }
            }
        }

        console.log(`\n=== PGD SEARCH COMPLETE ===`);
        console.log(`Scanned ${pagesScanned} pages (${(pagesScanned * 4 / 1024).toFixed(0)}MB)`);
        console.log(`Found ${candidatesFound} PGD candidates total`);

        // Sort by score and show top 10
        pgdCandidates.sort((a, b) => b.score - a.score);
        console.log(`\nTop 10 PGD candidates:`);
        for (let i = 0; i < Math.min(10, pgdCandidates.length); i++) {
            const c = pgdCandidates[i];
            console.log(`  ${i+1}. 0x${c.addr.toString(16)}: score=${c.score}, kernel=${c.kernelEntries}, user=${c.userEntries}`);
        }

        return pgdCandidates;
    }
}