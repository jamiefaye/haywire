/**
 * Kernel Discovery for Paged Memory
 * Works with PagedMemory to handle large files without array size limits
 */

import { PagedMemory } from './paged-memory';
import { KernelConstants, ProcessInfo, PTE, MemorySection, DiscoveryStats, DiscoveryOutput, stripPAC } from './kernel-discovery';
import { PageCollection, PageInfo } from './page-info';
import { PageCacheDiscovery } from './page-cache-discovery';
import { KernelMem } from './kernel-mem';
import { VirtualAddress, VA } from './types/virtual-address';
import { PhysicalAddress, PA } from './types/physical-address';
// import { PageTableEntry } from './types/page-table'; // Unused

// Export the types we use
export type { ProcessInfo, PTE, MemorySection, DiscoveryStats, DiscoveryOutput, PageInfo };
export { KernelConstants, PageCollection };

const KNOWN_PROCESSES = [
    'init', 'systemd', 'kthreadd', 'rcu_gp', 'migration',
    'ksoftirqd', 'kworker', 'kcompactd', 'khugepaged',
    'kswapd', 'kauditd', 'sshd', 'systemd-journal',
    'systemd-resolved', 'systemd-networkd', 'bash',
    'vlc', 'firefox', 'chrome', 'code', 'vim', 'emacs'
];

export class PagedKernelDiscovery {
    private memory: PagedMemory;
    private kmem: KernelMem;
    private decoder = new TextDecoder('ascii');
    private totalSize: number = 0;

    // Data structures
    private processes = new Map<number, ProcessInfo>();
    private kernelPtes: PTE[] = [];
    private pageToPids = new Map<string, Set<number>>();  // Using string key for PA comparison
    private zeroPages = new Set<string>();  // Using string for PA
    private swapperPgDir: PhysicalAddress = PA(0);
    private kernelPgdPA: PhysicalAddress | null = null;

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
        this.kmem = new KernelMem(memory);
    }

    /**
     * Check if value looks like a kernel pointer
     */
    private isKernelPointer(value: bigint): boolean {
        return (value >> 48n) === 0xffffn;
    }

    /**
     * Debug helper: dump PUD table contents
     */
    private dumpPudTable(pudTablePA: PhysicalAddress, pgdIndex: number): void {
        const pudOffset = Number(pudTablePA) - Number(KernelConstants.GUEST_RAM_START);
        if (pudOffset < 0 || pudOffset >= this.totalSize) {
//             console.log(`      PUD table PA 0x${pudTablePA.toString(16)} is outside memory range`);
            return;
        }

        const pudData = this.memory.readBytes(pudOffset, 512 * 8);
        if (!pudData) {
            console.log(`      Failed to read PUD table at PA 0x${pudTablePA.toString(16)}`);
            return;
        }

        const view = new DataView(pudData.buffer, pudData.byteOffset, pudData.byteLength);
        // Count non-zero entries without dumping everything
        let nonZeroEntries = 0;
        const nonZeroIndices: number[] = [];
        for (let i = 0; i < 512; i++) {
            const entry = view.getBigUint64(i * 8, true);
            if (entry !== 0n) {
                nonZeroEntries++;
                if (nonZeroIndices.length < 10) { // Only track first 10
                    nonZeroIndices.push(i);
                }
            }
        }

        // Just show summary instead of dumping all 512 entries
//         console.log(`      PUD table at PA 0x${pudTablePA.toString(16)}: ${nonZeroEntries} non-zero entries`);
        if (nonZeroIndices.length > 0) {
//             console.log(`        Non-zero at indices: ${nonZeroIndices.join(', ')}${nonZeroEntries > 10 ? '...' : ''}`);
        }
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

        // Debug: Look for Firefox and other interesting processes
        if (pid === 6969 || name.toLowerCase().includes('firefox') ||
            name.toLowerCase().includes('vlc') || name.toLowerCase().includes('chrome')) {
//             console.log(`*** FOUND ${name}! PID=${pid} at offset 0x${offset.toString(16)}`);
        }

        // Get mm_struct pointer
        const mmRaw = this.memory.readU64(offset + KernelConstants.MM_OFFSET) || 0n;
        // Strip pointer authentication code if present
        const mmStripped = stripPAC(VA(mmRaw));

        // Debug processes to see what's happening with mm field
        // Disable debug output while focusing on PGDs
        const DEBUG_PIDS = false;
        if (DEBUG_PIDS) {
            const debugCount = Array.from(this.processes.values()).length;
            // Look for interesting processes
            const isInteresting = name && (name.includes('systemd') || name.includes('ssh') || name.includes('bash'));
            if ((debugCount < 10 || isInteresting) && pid > 0 && pid < 100000 && name && name.length > 0) {
//                 console.log(`\n  [DEBUG] PID ${pid} (${name}):`);
//                 console.log(`    task_struct offset: 0x${offset.toString(16)}`);
//                 console.log(`    mm field offset: 0x${(offset + KernelConstants.MM_OFFSET).toString(16)} (task + 0x${KernelConstants.MM_OFFSET.toString(16)})`);
//                 console.log(`    Raw mm value: 0x${mmRaw.toString(16)}`);

                // Read some context around the mm field
                const contextStart = offset + KernelConstants.MM_OFFSET - 16;
                const contextBytes = this.memory.readBytes(contextStart, 48);
                if (contextBytes) {
//                     console.log(`    Context around mm field:`);
                    for (let i = 0; i < 48; i += 8) {
                        const val = new DataView(contextBytes.buffer, contextBytes.byteOffset + i, 8).getBigUint64(0, true);
                        const marker = (i === 16) ? ' <- mm field' : '';
//                         console.log(`      [${(contextStart + i).toString(16)}]: 0x${val.toString(16).padStart(16, '0')}${marker}`);
                    }
                }

                if (mmRaw !== mmStripped) {
//                     console.log(`    Stripped: 0x${mmStripped.toString(16)} (PAC removed)`);
                }
            }
        }

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
        const isKernel = mmStripped === VA(0);

        // Log successful process discovery
        if (pid === 1545 || (name && name.includes('dbus'))) {
//             console.log(`Found process: PID ${pid}, name="${name || 'NULL'}", mm=0x${mmStripped.toString(16)}, kernel=${isKernel}`);
            if (pid === 1545 && name !== 'dbus-daemon') {
//                 console.log(`  WARNING: PID 1545 found but name is "${name}" not "dbus-daemon"`);
            }
        }

        // Basic requirements
        if (!name || pid < 1 || pid > 32768) {
            return null;
        }

        // Check if name is valid - be ULTRA strict
        if (!this.isPrintableString(name)) {
            return null;
        }

        // Reject very short names unless they're known
        if (name.length < 3 && !KNOWN_PROCESSES.includes(name)) {
            return null;  // Minimum 3 characters for unknown names
        }

        // Name should match expected pattern - EXTREMELY strict
        // Must start with letter or /, contain only alphanumeric/-/_/[/]/:/./$
        if (!/^[a-zA-Z\/][a-zA-Z0-9\-_\/\[\]:\.\$]*$/.test(name)) {
            return null;
        }

        // Additional check: require at least 2 alphanumeric characters
        const alphaCount = (name.match(/[a-zA-Z0-9]/g) || []).length;
        if (alphaCount < 2) {
            return null;
        }

        // Reject names that look like random garbage
        // Check for reasonable character distribution
        const upperCount = (name.match(/[A-Z]/g) || []).length;
        const lowerCount = (name.match(/[a-z]/g) || []).length;
        const digitCount = (name.match(/[0-9]/g) || []).length;

        // Reject if all uppercase or all lowercase (unless known)
        if (!isKnown && name.length > 2) {
            // Reject random-looking patterns
            if (upperCount > 0 && lowerCount > 0) {
                // Mixed case - check if it looks reasonable
                // Reject patterns like xP, sZ7v which have random case mixing
                const transitions = this.countCaseTransitions(name);
                if (transitions > name.length / 2) {
                    return null;  // Too many case transitions
                }
            }
        }

        // Much stricter requirement - need multiple indicators
        let validityScore = 0;
        if (isKnown) validityScore += 3;
        if (hasValidList) validityScore += 2;
        if (kernelPtrCount >= 5) validityScore += 2;
        if (kernelPtrCount >= 10) validityScore += 1;
        if (mmStripped === VA(0) || VirtualAddress.isKernel(mmStripped)) validityScore += 1;

        // Require higher score for acceptance
        if (validityScore < 3) {
            return null;
        }

        // mm_struct validation - should be 0 (kernel thread) or kernel VA (user process)
        if (mmStripped !== VA(0)) {
            // For user processes, mm_struct should be a kernel VA (0xffff...)
            if (!VirtualAddress.isKernel(mmStripped)) {
                // Not a kernel VA and not zero - invalid
                return null;
            }
        }

        // Get PGD if mm_struct is valid
        let pgdPtr = PA(0);
        if (mmStripped && mmStripped >= Number(KernelConstants.GUEST_RAM_START) && mmStripped < Number(KernelConstants.GUEST_RAM_END)) {
            const mmOffset = Number(mmStripped) - Number(KernelConstants.GUEST_RAM_START);
            const pgdRaw = this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM);
            if (pgdRaw) {
                pgdPtr = PA(pgdRaw);
            }
        }

        return {
            pid,
            name,
            taskStruct: PA(Number(KernelConstants.GUEST_RAM_START) + offset),
            mmStruct: VA(mmStripped),  // Virtual address for mm_struct
            pgd: pgdPtr,
            isKernelThread: isKernel,
            tasksNext: PA(tasksNext),
            tasksPrev: PA(tasksPrev),
            ptes: [],
            sections: [],
        };
    }

    /**
     * Count case transitions in a string (for detecting random patterns)
     */
    private countCaseTransitions(str: string): number {
        let transitions = 0;
        for (let i = 1; i < str.length; i++) {
            const prevIsUpper = /[A-Z]/.test(str[i-1]);
            const currIsUpper = /[A-Z]/.test(str[i]);
            const prevIsLower = /[a-z]/.test(str[i-1]);
            const currIsLower = /[a-z]/.test(str[i]);

            if ((prevIsUpper && currIsLower) || (prevIsLower && currIsUpper)) {
                transitions++;
            }
        }
        return transitions;
    }

    /**
     * Search specifically for a PID value in memory
     */
    private searchForSpecificPID(targetPID: number, totalSize: number): void {
//         console.log(`\nSearching for PID ${targetPID} in memory...`);
        let foundCount = 0;
        const locations: number[] = [];

        // Search entire memory for this PID value
        for (let offset = 0; offset < totalSize - 4; offset += 4) {
            if (offset % (500 * 1024 * 1024) === 0 && offset > 0) {
//                 console.log(`  Scanned ${offset / (1024 * 1024)}MB...`);
            }

            const value = this.memory.readU32(offset);
            if (value === targetPID) {
                // Found the PID value, check if it could be in a task_struct
                // PID is at offset 0x750, so task_struct would start at offset - 0x750
                const possibleTaskOffset = offset - KernelConstants.PID_OFFSET;

                if (possibleTaskOffset >= 0 && possibleTaskOffset < totalSize - KernelConstants.TASK_STRUCT_SIZE) {
                    // Read comm field to see if it's valid
                    const name = this.memory.readString(possibleTaskOffset + KernelConstants.COMM_OFFSET);
                    const mmRaw = this.memory.readU64(possibleTaskOffset + KernelConstants.MM_OFFSET) || 0n;
                    const tgid = this.memory.readU32(possibleTaskOffset + KernelConstants.PID_OFFSET + 4);

//                     console.log(`\n  Found PID ${targetPID} at offset 0x${offset.toString(16)}`);
//                     console.log(`    Possible task_struct at 0x${possibleTaskOffset.toString(16)}`);
//                     console.log(`    Physical address: 0x${(KernelConstants.GUEST_RAM_START + possibleTaskOffset).toString(16)}`);
//                     console.log(`    Comm field: "${name || 'null'}" (length: ${name?.length || 0})`);
//                     console.log(`    TGID field: ${tgid}`);
//                     console.log(`    MM field: 0x${mmRaw.toString(16)}`);

                    // If this looks like a valid task_struct, add it to our process list
                    if (name && this.isPrintableString(name) && tgid && tgid > 0 && tgid < 100000) {
//                         console.log(`    ✓ This looks like a valid task_struct!`);
                        const process = this.checkTaskStruct(possibleTaskOffset);
                        if (process && !this.processes.has(process.pid)) {
//                             console.log(`    ✓✓ Added to process list!`);
                            this.processes.set(process.pid, process);
                        }
                    }

                    locations.push(offset);
                    foundCount++;
                    if (foundCount >= 10) break;  // Limit to first 10 occurrences
                }
            }
        }

//         console.log(`\nFound ${foundCount} occurrences of PID ${targetPID}`);
        if (locations.length > 0) {
//             console.log('Locations:', locations.map(l => '0x' + l.toString(16)).join(', '));
        }
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
                if (physAddr > KernelConstants.GUEST_RAM_END) { // Beyond guest RAM
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
     * Find a specific PID by searching for it byte-by-byte
     * This catches PIDs that might be in SLUB allocations straddling page boundaries
     */
    private findSpecificPID(targetPID: number, totalSize: number): void {
        let foundCount = 0;

        // Search page by page, checking all possible alignments including cross-page boundaries
        for (let pageStart = 0; pageStart < totalSize; pageStart += KernelConstants.PAGE_SIZE) {
            // Check every 8-byte aligned offset in the page (task_structs are aligned)
            for (let offset = pageStart; offset < Math.min(pageStart + KernelConstants.PAGE_SIZE + 0x800, totalSize - 4); offset += 8) {
                const pid = this.memory.readU32(offset);
                if (pid === targetPID) {
                    // PID is at offset 0x750, so task_struct would start at offset - 0x750
                    const possibleTaskOffset = offset - KernelConstants.PID_OFFSET;

                    if (possibleTaskOffset >= 0 && possibleTaskOffset < totalSize - KernelConstants.TASK_STRUCT_SIZE) {
                        // Read comm field to see if it's valid
                        const name = this.memory.readString(possibleTaskOffset + KernelConstants.COMM_OFFSET);
                        const mmRaw = this.memory.readU64(possibleTaskOffset + KernelConstants.MM_OFFSET) || 0n;
                        const tgid = this.memory.readU32(possibleTaskOffset + KernelConstants.PID_OFFSET + 4);

//                         console.log(`\n  Found PID ${targetPID} at offset 0x${offset.toString(16)}`);
//                         console.log(`    Possible task_struct at 0x${possibleTaskOffset.toString(16)}`);
//                         console.log(`    Physical address: 0x${(KernelConstants.GUEST_RAM_START + possibleTaskOffset).toString(16)}`);
//                         console.log(`    Comm field: "${name || 'null'}" (length: ${name?.length || 0})`);
//                         console.log(`    TGID field: ${tgid}`);
//                         console.log(`    MM field: 0x${mmRaw.toString(16)}`);

                        // If this looks like a valid task_struct, add it to our process list
                        if (name && this.isPrintableString(name) && tgid && tgid > 0 && tgid < 100000) {
//                             console.log(`    ✓ VALID TASK_STRUCT FOR PID ${targetPID}!`);
                            const process = this.checkTaskStruct(possibleTaskOffset);
                            if (process) {
                                this.processes.set(process.pid, process);
                                foundCount++;
                            }
                        }
                    }
                }
            }
        }

        if (foundCount === 0) {
//             console.log(`  ⚠️  PID ${targetPID} not found in memory`);
        } else {
//             console.log(`  ✓ Found ${foundCount} instances of PID ${targetPID}`);
        }
    }

    /**
     * Find all processes by scanning for task_structs
     */
    private findProcesses(totalSize: number): void {
        // Use the library version
        if (this.kernelPgdPA) {
            this.kmem.setKernelPgd(Number(this.kernelPgdPA));
        }

        this.processes = this.kmem.findProcesses();

        this.stats.totalProcesses = this.processes.size;
        this.stats.kernelThreads = Array.from(this.processes.values())
            .filter(p => p.isKernelThread).length;
        this.stats.userProcesses = this.stats.totalProcesses - this.stats.kernelThreads;

        const withMm = Array.from(this.processes.values()).filter(p => p.mmStruct && p.mmStruct > 0xffff000000000000n);
    }

    /**
     * Find all swapper_pg_dir candidates
     */
    private findSwapperPgDirCandidates(): Array<{addr: PhysicalAddress, score: number, kernelEntries: number, userEntries: number}> {
//         console.log('Finding swapper_pg_dir candidates...');
        const candidates: Array<{addr: PhysicalAddress, score: number, kernelEntries: number, userEntries: number}> = [];

        // Common locations for swapper_pg_dir (relative to GUEST_RAM_START)
        const potentialLocations = [
            0x082c00000 - Number(KernelConstants.GUEST_RAM_START),  // Previously found location
            0x041000000 - Number(KernelConstants.GUEST_RAM_START),  // Recent finding
            0x042c00000 - Number(KernelConstants.GUEST_RAM_START),  // Alternative
            0x463d4000 - Number(KernelConstants.GUEST_RAM_START),   // Latest finding
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
//                     console.log(`  Known location candidate at 0x${candidate.addr.toString(16)}: score=${candidate.score}`);
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
//                         console.log(`  High-score candidate at 0x${candidate.addr.toString(16)}: score=${candidate.score}`);
                    }
                }
            }
        }

        return candidates;
    }

    /**
     * Evaluate if an offset could be a PGD by chasing pointers
     */
    private evaluatePgdCandidate(offset: number): {addr: PhysicalAddress, score: number, kernelEntries: number, userEntries: number} | null {
        let kernelEntries = 0;
        let userEntries = 0;
        let validChains = 0;  // Count of successfully chased pointer chains

        // Special debug for the real PGD and extracted PGDs
        const isRealPGD = offset === 0xf6dbf000;
        const isExtractedPGD = offset === 0x228d000 || offset === 0x2c40000; // 0x4228d000 - GUEST_RAM_START
        if (isRealPGD) {
//             console.log('  DEBUG: Evaluating REAL PGD at offset 0xf6dbf000');
        }
        if (isExtractedPGD) {
//             console.log(`  DEBUG: Evaluating extracted PGD at offset 0x${offset.toString(16)} (PA: 0x${(offset + KernelConstants.GUEST_RAM_START).toString(16)})`);
        }

        // Read the page
        const pageBuffer = this.memory.readBytes(offset, KernelConstants.PAGE_SIZE);
        if (!pageBuffer) {
//             if (isRealPGD) console.log('    Failed: Could not read page buffer');
            return null;
        }

        const view = new DataView(pageBuffer.buffer, pageBuffer.byteOffset, pageBuffer.byteLength);

        // Check a sample of entries by chasing their chains
        const entriesToTest = [0, 1, 256, 257, 258, 300, 400, 500]; // Mix of user and kernel

        for (const i of entriesToTest) {
            // Use getBigUint64 to avoid precision issues with large addresses
            const entryBig = view.getBigUint64(i * 8, true);

            // For checking low bits, we can safely use Number() since we only care about bits [1:0]
            const lowBits = Number(entryBig & 0x3n);

            // Check if this is a valid table descriptor (bits [1:0] = 0b11)
            if (lowBits !== 0x3) {
                if ((isRealPGD || isExtractedPGD) && (i === 0 || i === 256)) {
//                     console.log(`    PGD[${i}]: entry=0x${entryBig.toString(16)}, bits[1:0]=${lowBits.toString(2)} (need 0b11)`);
                }
                continue;
            }

            // Count the entry
            if (i < 256) userEntries++;
            else kernelEntries++;

            // Try to chase the pointer chain
            // Extract physical address from bits [47:12] - mask off high and low bits
            // ARM64 format: bits [63:48] are attributes, [47:12] are PA, [11:0] are flags
            const pudAddrBig = (entryBig & 0x0000FFFFFFFFF000n);  // Mask to get bits [47:12]
            const pudAddr = Number(pudAddrBig);  // Safe to convert for address comparisons

            // Allow PUD addresses outside GUEST_RAM range since they may be in extended RAM
            // But still reject clearly invalid addresses
            if (pudAddr < 0x1000 || pudAddr > 0x200000000) { // Reject if below 4KB or above 8GB
                if ((isRealPGD || isExtractedPGD) && (i === 0 || i === 256)) {
//                     console.log(`    PGD[${i}]: entry=0x${entryBig.toString(16)}, pudAddr=0x${pudAddrBig.toString(16)}`);
//                     console.log(`    Rejecting: pudAddr clearly invalid (< 4KB or > 8GB)`);
                }
                continue;
            }

            // Skip reading PUD if it's outside our accessible memory file range
            if (pudAddr < KernelConstants.GUEST_RAM_START || pudAddr >= KernelConstants.GUEST_RAM_END) {
                // Can't verify the PUD table, but don't reject the PGD entirely
                // Just count it as a valid entry without chain verification
                if (i < 256) userEntries++;
                else kernelEntries++;
                continue;
            }

            // Read the PUD table and check if it looks valid
            const pudOffset = pudAddr - Number(KernelConstants.GUEST_RAM_START);
            const pudData = this.memory.readBytes(pudOffset, 64); // Read first 8 entries
            if (!pudData) continue;

            const pudView = new DataView(pudData.buffer, pudData.byteOffset, pudData.byteLength);
            let validPudEntries = 0;

            for (let j = 0; j < 8; j++) {
                // Use getBigUint64 to avoid precision issues
                const pudEntryBig = pudView.getBigUint64(j * 8, true);
                const pudLowBits = Number(pudEntryBig & 0x3n);

                if (pudLowBits === 3) {
                    // Extract PA from bits [47:12]
                    const pmdAddrBig = pudEntryBig & 0x0000FFFFFFFFF000n;
                    const pmdAddr = Number(pmdAddrBig);
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
        if (validChains < 1) {
            if (isRealPGD || isExtractedPGD) {
//                 console.log(`    Failed: validChains=${validChains} (need >=1), kernel=${kernelEntries}, user=${userEntries}`);
            }
            return null;
        }

        // Count all kernel entries for scoring
        let totalKernelEntries = 0;
        let totalUserEntries = 0;
        for (let i = 0; i < 512; i++) {
            // Use getBigUint64 for accurate reading
            const entryBig = view.getBigUint64(i * 8, true);
            const lowBits = Number(entryBig & 0x3n);

            if (lowBits === 3) {
                if (i < 256) totalUserEntries++;
                else totalKernelEntries++;
            }
        }

        // Score based on valid chains and entry distribution
        let score = validChains * 50;  // Heavy weight on valid chains
        if (totalKernelEntries >= 15) score += totalKernelEntries * 2;
        if (totalUserEntries <= 5) score += (5 - totalUserEntries) * 5;

        if (isExtractedPGD) {
//             console.log(`    Success: validChains=${validChains}, totalKernelEntries=${totalKernelEntries}, totalUserEntries=${totalUserEntries}, score=${score}`);
        }

        return {
            addr: PA(offset + Number(KernelConstants.GUEST_RAM_START)),
            score,
            kernelEntries: totalKernelEntries,
            userEntries: totalUserEntries
        };
    }

    /**
     * Find swapper_pg_dir by adaptive signature with scoring
     * Returns the physical address of the best candidate or 0 if not found
     */
    private findSwapperPgdByAdaptiveSignature(): number {
//         console.log('Searching for swapper_pg_dir by adaptive signature...');

        // Detect estimated RAM size from file
        const estimatedRamSize = Math.ceil((this.totalSize) / (1024*1024*1024));
//         console.log(`  Estimated VM RAM size: ~${estimatedRamSize}GB`);

        // Scan focused regions where swapper_pg_dir is typically found
        // Limit to smaller ranges to avoid memory issues
        const scanRegions = [
            [0x130000000, 0x138000000], // 4.75-4.875GB range (includes known 0x136deb000)
            [0x138000000, 0x140000000], // 4.875-5GB range
            [0x78000000, 0x80000000],   // 1.875-2GB range (fallback)
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
        let pagesScanned = 0;
        const maxCandidates = 10; // Limit candidates to avoid memory buildup

        for (const [regionStart, regionEnd] of scanRegions) {
            const start = Math.max(0, regionStart - Number(KernelConstants.GUEST_RAM_START));
            const end = Math.min(this.totalSize, regionEnd - Number(KernelConstants.GUEST_RAM_START));
            if (start >= end) continue;

//             console.log(`  Scanning region 0x${regionStart.toString(16)}-0x${regionEnd.toString(16)}...`);

            // Process in smaller chunks to avoid memory issues
            const chunkSize = 16 * 1024 * 1024; // 16MB chunks
            for (let chunkStart = start; chunkStart < end && candidates.length < maxCandidates; chunkStart += chunkSize) {
                const chunkEnd = Math.min(chunkStart + chunkSize, end);

                for (let offset = chunkStart; offset < chunkEnd && candidates.length < maxCandidates; offset += KernelConstants.PAGE_SIZE) {
                    pagesScanned++;

                    // Quick check for sparse page using single read
                    const pageData = this.memory.read(offset, KernelConstants.PAGE_SIZE);
                    if (!pageData) continue;

                    // Count non-zero entries efficiently
                    let nonZeroEntries = 0;
                    const view = new DataView(pageData.buffer, pageData.byteOffset, pageData.byteLength);
                    for (let i = 0; i < 512; i++) {
                        const entry = view.getBigUint64(i * 8, true);
                        if (entry !== 0n) {
                            nonZeroEntries++;
                            if (nonZeroEntries > 20) break;
                        }
                    }

                    // Only analyze sparse pages (2-20 entries)
                    if (nonZeroEntries >= 2 && nonZeroEntries <= 20) {
                        const physAddr = offset + Number(KernelConstants.GUEST_RAM_START);
                        const candidate = this.analyzeSwapperCandidate(offset, physAddr);
                        if (candidate && candidate.score >= 3) {
                            candidates.push(candidate);
//                             console.log(`    Found candidate at PA 0x${physAddr.toString(16)} (score: ${candidate.score})`);
                        }
                    }
                }
            }

            if (candidates.length >= maxCandidates) {
//                 console.log(`  Stopping early - found ${maxCandidates} candidates`);
                break;
            }
        }

//         console.log(`  Scanned ${pagesScanned} pages`);

        // Sort by score
        candidates.sort((a, b) => b.score - a.score);

//         console.log(`  Found ${candidates.length} high-scoring candidates`);

        if (candidates.length > 0) {
            // Show top candidates
            const top = candidates.slice(0, 5);
            for (const c of top) {
//                 console.log(`    PA: 0x${c.physAddr.toString(16)}, Score: ${c.score}, RAM: ${c.memSizeEstimate}GB, User: ${c.userEntries}, Kernel: ${c.kernelEntries.length}`);
            }

            const best = candidates[0];
//             console.log(`  ✓ Best candidate at PA 0x${best.physAddr.toString(16)} (score: ${best.score}/7, est. RAM: ${best.memSizeEstimate}GB)`);
            return best.physAddr;
        }

//         console.log('  swapper_pg_dir not found by signature scan');
        return 0;
    }

    /**
     * Analyze a potential swapper_pg_dir candidate with adaptive scoring
     */
    private analyzeSwapperCandidate(offset: number, physAddr: number): any {
        if (offset + KernelConstants.PAGE_SIZE > this.totalSize) {
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
            const entry = this.memory.readU64(offset + i * 8);
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
            const pudOffset = pgd0Entry.pa - Number(KernelConstants.GUEST_RAM_START);
            if (pudOffset >= 0 && pudOffset + KernelConstants.PAGE_SIZE <= this.totalSize) {
                let pudCount = 0;
                let consecutivePuds = true;

                for (let j = 0; j < 512; j++) {
                    const pud = this.memory.readU64(pudOffset + j * 8);
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
     * Find best swapper_pg_dir from candidates
     */
    private findSwapperPgDir(): PhysicalAddress {
        const candidates = this.findSwapperPgDirCandidates();

        if (candidates.length === 0) {
//             console.log('No swapper_pg_dir candidates found');
            return PA(0);
        }

        // Sort by score
        candidates.sort((a, b) => b.score - a.score);

//         console.log(`Found ${candidates.length} swapper_pg_dir candidates`);

        // Debug: Show what processes we're working with
        let processCount = 0;
//         console.log('Sample processes with kernel mm_structs:');
        for (const process of this.processes.values()) {
            if (process.mmStruct && process.mmStruct > 0xffff000000000000) {
//                 console.log(`  ${process.name}: mm_struct=0x${process.mmStruct.toString(16)}`);
                processCount++;
                if (processCount >= 5) break;
            }
        }

        // First, specifically test our known good candidate
        const knownGood = PA(0x463d4000);
//         console.log(`Testing known candidate: ${PhysicalAddress.toHex(knownGood)}`);
        if (this.validateSwapperPgDir(knownGood)) {
//             console.log(`  ✓ VALIDATED - Known candidate works!`);
            return knownGood;
        } else {
//             console.log(`  ✗ Known candidate failed validation`);
        }

        // Test top candidates with actual VA translation
        let validatedCandidate = PA(0);
        for (let i = 0; i < Math.min(10, candidates.length); i++) {
            const c = candidates[i];
//             console.log(`Testing candidate #${i+1}: ${PhysicalAddress.toHex(c.addr)} (score: ${c.score}`);

            if (this.validateSwapperPgDir(c.addr)) {
//                 console.log(`  ✓ VALIDATED - This appears to be the correct swapper_pg_dir!`);
                validatedCandidate = c.addr;
                break;
            } else {
//                 console.log(`  ✗ Failed validation`);
            }
        }

        if (validatedCandidate === PA(0)) {
//             console.log('Warning: No candidates passed validation, using highest score');
            validatedCandidate = candidates[0].addr;
        }

//         console.log(`Selected swapper_pg_dir: ${PhysicalAddress.toHex(validatedCandidate)}`);
        return validatedCandidate;
    }

    /**
     * Find page tables directly by scanning memory
     */
    private findPageTables(totalSize: number): void {
//         console.log('Finding page tables...');
        let pteTableCount = 0;

        // Scan the entire memory space, with progress updates every 500MB
        const chunkSize = 500 * 1024 * 1024; // 500MB chunks for progress reporting

        for (let chunkStart = 0; chunkStart < totalSize; chunkStart += chunkSize) {
            const chunkEnd = Math.min(chunkStart + chunkSize, totalSize);

//             console.log(`  Scanning ${(chunkStart / (1024*1024)).toFixed(0)}-${(chunkEnd / (1024*1024)).toFixed(0)}MB...`);
            let pageCount = 0;

            for (let offset = chunkStart; offset < chunkEnd - KernelConstants.PAGE_SIZE; offset += KernelConstants.PAGE_SIZE) {
                pageCount++;
                if (this.checkPteTable(offset)) {
                    pteTableCount++;
                    const absoluteAddr = offset + Number(KernelConstants.GUEST_RAM_START);

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
//                 console.log(`    Found ${pteTableCount} page tables so far`);
            }
        }

//         console.log(`Found ${pteTableCount} page tables total`);
        this.stats.kernelPTEs = pteTableCount;
    }

    private simpleTranslationFailures = 0;
    private simpleTranslationSuccesses = 0;
    private maxSimpleTranslationFailureLogs = 10;

    /**
     * Simple translation for kernel VAs in the 0xffff0000xxxxxxxx range
     * Just masks out the top 16 bits to get physical address
     */
    private simpleTranslateKernelVA(va: bigint | number): number | null {
        const virtualAddr = typeof va === 'number' ? BigInt(va) : va;

        // Check if it's a kernel VA in the expected range
        if ((virtualAddr >> 32n) !== 0xffff0000n) {
            if (this.simpleTranslationFailures < this.maxSimpleTranslationFailureLogs) {
//                 console.log(`  [SimpleTranslate] Failed: VA 0x${virtualAddr.toString(16)} not in 0xffff0000xxxxxxxx range`);
                this.simpleTranslationFailures++;
            }
            return null; // Not in the simple translation range
        }

        // Simple mask - just take lower 48 bits
        const physAddr = virtualAddr & 0xFFFFFFFFFFFFn;

        // Verify it's within our memory file range
        // PagedMemory stores totalSize as a private field, we need to use the totalSize passed to constructor
        if (physAddr >= BigInt(this.totalSize)) {
            if (this.simpleTranslationFailures < this.maxSimpleTranslationFailureLogs) {
//                 console.log(`  [SimpleTranslate] Failed: PA 0x${physAddr.toString(16)} exceeds file size (${(this.totalSize / (1024*1024*1024)).toFixed(2)} GB)`);
                this.simpleTranslationFailures++;
            }
            return null;
        }

        this.simpleTranslationSuccesses++;
        if (this.simpleTranslationSuccesses <= 5) {
//             console.log(`  [SimpleTranslate] Success #${this.simpleTranslationSuccesses}: VA 0x${virtualAddr.toString(16)} -> PA 0x${physAddr.toString(16)}`);
        }

        return Number(physAddr);
    }

    /**
     * Helper to translate VA with a specific PGD
     * Temporarily sets the PGD, does the translation, then restores
     */
    private translateWithPgd(va: bigint | number, pgdBase: number | PhysicalAddress): number | null {
        const virtualAddr = typeof va === 'number' ? BigInt(va) : va;
        const oldPgd = this.kmem.getKernelPgd();
        this.kmem.setKernelPgd(typeof pgdBase === 'number' ? pgdBase : Number(pgdBase));
        const result = this.kmem.translateVA(virtualAddr);
        this.kmem.setKernelPgd(oldPgd);
        return result;
    }


    /**
     * Validate a swapper_pg_dir candidate by testing the full chain
     */
    private validateSwapperPgDir(pgdAddr: PhysicalAddress): boolean {
        let successCount = 0;
        let failCount = 0;
        let debugFirst = true;

        // Test with known kernel VAs from processes
        for (const process of this.processes.values()) {
            if (process.mmStruct && process.mmStruct > 0xffff000000000000) {
                // Step 1: Use candidate PGD to translate mm_struct VA→PA
                const mmPa = this.translateWithPgd(BigInt(process.mmStruct), Number(pgdAddr));

                if (debugFirst) {
//                     console.log(`    Testing ${process.name}:`);
//                     console.log(`      mm_struct VA: 0x${process.mmStruct.toString(16)}`);
                    if (mmPa) {
//                         console.log(`      → mm_struct PA: 0x${mmPa.toString(16)}`);
                    } else {
//                         console.log(`      → Translation failed`);
                        this.debugTranslateVA(BigInt(process.mmStruct), Number(pgdAddr));
                        debugFirst = false;
                        failCount++;
                        continue;
                    }
                }

                if (!mmPa || mmPa < Number(KernelConstants.GUEST_RAM_START) || mmPa >= Number(KernelConstants.GUEST_RAM_END)) {
                    failCount++;
                    continue;
                }

                // Step 2: Read PGD pointer from mm_struct
                const mmOffset = mmPa - Number(KernelConstants.GUEST_RAM_START);
                const processPgdPtr = this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM);
                if (!processPgdPtr) {
                    failCount++;
                    continue;
                }

                const processPgd = Number(processPgdPtr);
                if (debugFirst) {
//                     console.log(`      Process PGD from mm_struct: 0x${processPgd.toString(16)}`);
                }

                // Step 3: Validate the process PGD looks reasonable
                if (processPgd < Number(KernelConstants.GUEST_RAM_START) || processPgd >= Number(KernelConstants.GUEST_RAM_END)) {
                    if (debugFirst) {
//                         console.log(`      → Invalid PGD address`);
                    }
                    failCount++;
                    continue;
                }

                // Step 4: Ultimate test - use process PGD to translate its own mm_struct
                const verifyPa = this.translateWithPgd(BigInt(process.mmStruct), processPgd);
                if (verifyPa === mmPa) {
                    if (debugFirst) {
//                         console.log(`      ✓ Process PGD successfully translates its own mm_struct!`);
                    }
                    successCount++;
                    process.pgd = processPgd;
                } else {
                    if (debugFirst) {
//                         console.log(`      ✗ Process PGD failed to translate correctly`);
//                         console.log(`        Expected PA: 0x${mmPa.toString(16)}, got: ${verifyPa ? '0x' + verifyPa.toString(16) : 'null'}`);
                    }
                    failCount++;
                }

                debugFirst = false;

                // Test a few to get a sample
                if (successCount + failCount >= 5) break;
            }
        }

        if (successCount + failCount === 0) {
//             console.log(`    No kernel VAs found to test`);
            return false;
        }

        const successRate = successCount / (successCount + failCount);
//         console.log(`    Final: ${successCount}/${successCount + failCount} full chains validated (${(successRate * 100).toFixed(0)}%)`);
        return successRate > 0.6;  // Lower threshold since this is harder
    }

    /**
     * Debug version of translateVA to see where it fails
     */
    private debugTranslateVA(va: number | bigint, pgdBase: number): void {
        const virtualAddr = typeof va === 'number' ? BigInt(va) : va;

        // Extract indices - standard 4-level page table walk
        const pgdIndex = Number((virtualAddr >> 39n) & 0x1FFn);  // bits 47-39
        const pudIndex = Number((virtualAddr >> 30n) & 0x1FFn);  // bits 38-30
        const pmdIndex = Number((virtualAddr >> 21n) & 0x1FFn);  // bits 29-21
        const pteIndex = Number((virtualAddr >> 12n) & 0x1FFn);  // bits 20-12

//         console.log(`      Indices: PGD[${pgdIndex}] PUD[${pudIndex}] PMD[${pmdIndex}] PTE[${pteIndex}]`);

        // Read PGD entry
        const pgdOffset = pgdBase - Number(KernelConstants.GUEST_RAM_START) + (pgdIndex * 8);
        const pgdEntry = this.memory.readU64(pgdOffset);
//         console.log(`      PGD entry at 0x${(pgdBase + pgdIndex * 8).toString(16)}: ${pgdEntry ? '0x' + pgdEntry.toString(16) : 'null'}`);

        // Check validity using BigInt to avoid precision loss
        const pgdTypeBits = pgdEntry ? pgdEntry & 3n : 0n;
//         console.log(`      Entry type bits [1:0]: ${pgdTypeBits} (binary: 0b${pgdTypeBits.toString(2)})`);
        if (!pgdEntry || pgdTypeBits !== 3n) {
//             console.log(`      → PGD entry invalid or not present`);
            return;
        }
//         console.log(`      → PGD entry is valid table descriptor`);

        const pudBase = Number(pgdEntry & 0x0000FFFFFFFFF000n);  // Extract bits [47:12] with BigInt
        const pudOffset = pudBase - Number(KernelConstants.GUEST_RAM_START) + (pudIndex * 8);
        const pudEntry = this.memory.readU64(pudOffset);
//         console.log(`      PUD entry at 0x${(pudBase + pudIndex * 8).toString(16)}: ${pudEntry ? '0x' + pudEntry.toString(16) : 'null'}`);

        // Check validity using BigInt to avoid precision loss
        const pudTypeBits = pudEntry ? pudEntry & 3n : 0n;
        if (!pudEntry || pudTypeBits !== 3n) {
//             console.log(`      → PUD entry invalid or not present (type bits: ${pudTypeBits})`);
            return;
        }
//         console.log(`      → PUD entry is valid table descriptor`);

        // Continue to PMD level
        const pmdBase = Number(pudEntry & 0x0000FFFFFFFFF000n);  // Extract bits [47:12] with BigInt
        const pmdOffset = pmdBase - Number(KernelConstants.GUEST_RAM_START) + (pmdIndex * 8);
        const pmdEntry = this.memory.readU64(pmdOffset);
//         console.log(`      PMD entry at 0x${(pmdBase + pmdIndex * 8).toString(16)}: ${pmdEntry ? '0x' + pmdEntry.toString(16) : 'null'}`);

        const pmdTypeBits = pmdEntry ? pmdEntry & 3n : 0n;
        if (!pmdEntry || pmdTypeBits !== 3n) {
//             console.log(`      → PMD entry invalid or not present (type bits: ${pmdTypeBits})`);
            return;
        }
//         console.log(`      → PMD entry is valid table descriptor`);

        // Continue to PTE level
        const pteBase = Number(pmdEntry & 0x0000FFFFFFFFF000n);  // Extract bits [47:12] with BigInt
        const pteOffset = pteBase - Number(KernelConstants.GUEST_RAM_START) + (pteIndex * 8);
        const pteEntry = this.memory.readU64(pteOffset);
//         console.log(`      PTE entry at 0x${(pteBase + pteIndex * 8).toString(16)}: ${pteEntry ? '0x' + pteEntry.toString(16) : 'null'}`);

        const pteTypeBits = pteEntry ? pteEntry & 3n : 0n;
        if (!pteEntry || pteTypeBits !== 3n) {
//             console.log(`      → PTE entry invalid or not present (type bits: ${pteTypeBits})`);
            return;
        }

        // Success! Extract final physical address
        const finalPA = Number(pteEntry & 0x0000FFFFFFFFF000n) + Number(virtualAddr & 0xFFFn);
//         console.log(`      ✓ SUCCESSFUL TRANSLATION: VA 0x${virtualAddr.toString(16)} → PA 0x${finalPA.toString(16)}`);
    }

    /**
     * Walk maple tree to find VMAs - implemented to match Linux kernel behavior
     */
    private walkMapleTree(nodePtr: bigint, sections: MemorySection[], depth: number): void {
        if (!nodePtr || nodePtr === 0n || depth > 15) {  // Increased depth limit to find deeper VMAs
            if (depth > 15 && sections.length === 0) {
//                 console.log(`  WARNING: Hit depth limit ${depth} without finding VMAs`);
            }
            return; // Prevent infinite recursion
        }

        // Maple nodes have type encoding in low 8 bits (MAPLE_NODE_MASK = 0xFF)
        // The kernel uses mte_to_node() to clean pointers: (void *)((unsigned long)entry & ~MAPLE_NODE_MASK)
        // Based on Linux kernel maple tree implementation:
        // - Bits 0-1: Node type (maple_dense=0, maple_leaf_64=1, maple_range_64=2, maple_arange_64=3)
        // - Bit 2: Reserved (for future use)
        // - Bits 3-7: Additional flags and metadata
        //
        // Common type values we see:
        // - 0x00: maple_dense (leaf node, stores values directly)
        // - 0x01: maple_leaf_64 (leaf node with 64 slots)
        // - 0x0c/0x1c: maple_range_64 variants (internal node, 16 slots)
        // - 0x1e: maple_arange_64 (internal node, 10 slots for allocation trees)
        // - 0x98, 0x99, etc: Leaf nodes with additional flags set
        const MAPLE_NODE_MASK = 0xFFn;
        const nodeType = Number(nodePtr & MAPLE_NODE_MASK);
        const cleanNodePtr = nodePtr & ~MAPLE_NODE_MASK;  // Same as mte_to_node() in kernel

        // Extract maple_type using kernel's exact method:
        // mte_node_type() = (entry >> MAPLE_NODE_TYPE_SHIFT) & MAPLE_NODE_TYPE_MASK
        const MAPLE_NODE_TYPE_SHIFT = 3;
        const MAPLE_NODE_TYPE_MASK = 0x0F;
        const mapleType = Number((nodePtr >> BigInt(MAPLE_NODE_TYPE_SHIFT)) & BigInt(MAPLE_NODE_TYPE_MASK));

        // enum maple_type values from kernel
        const MAPLE_DENSE = 0;
        const MAPLE_LEAF_64 = 1;
        const MAPLE_RANGE_64 = 2;
        const MAPLE_ARANGE_64 = 3;

        // THE CRITICAL TEST from kernel: ma_is_leaf(type) = type < maple_range_64
        const isLeafNode = mapleType < MAPLE_RANGE_64;  // type 0 or 1 = leaf
        const isInternalNode = !isLeafNode;              // type 2 or 3 = internal

        // Debug output
        if (depth <= 2 && sections.length < 20) {
            let nodeTypeStr: string;
            if (mapleType === MAPLE_DENSE) {
                nodeTypeStr = 'dense (leaf)';
            } else if (mapleType === MAPLE_LEAF_64) {
                nodeTypeStr = 'leaf_64 (leaf)';
            } else if (mapleType === MAPLE_RANGE_64) {
                nodeTypeStr = 'range_64 (internal)';
            } else if (mapleType === MAPLE_ARANGE_64) {
                nodeTypeStr = 'arange_64 (internal)';
            } else {
                nodeTypeStr = `unknown type ${mapleType}`;
            }
//             console.log(`${'  '.repeat(depth)}Maple node at depth ${depth}: 0x${nodePtr.toString(16)} (${nodeTypeStr})`);
        }

        // Translate kernel VA to physical address
        let actualNodePa: number;
        if ((cleanNodePtr & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
            const translated = this.translateWithPgd(cleanNodePtr, this.swapperPgDir);
            if (!translated) {
                return; // Can't translate
            }
            actualNodePa = translated;
        } else {
            // Check if the address is safe to convert to Number
            if (cleanNodePtr > 0x1FFFFFFFFFFFFFn) { // Larger than safe integer range
//                 console.log(`  Warning: Node pointer 0x${cleanNodePtr.toString(16)} exceeds safe integer range`);
                return;
            }
            actualNodePa = Number(cleanNodePtr);
        }

        if (actualNodePa < KernelConstants.GUEST_RAM_START || actualNodePa >= KernelConstants.GUEST_RAM_END) {
            return; // Invalid physical address
        }

        const nodeOffset = actualNodePa - Number(KernelConstants.GUEST_RAM_START);

        // CRITICAL DISTINCTION: Internal nodes vs Leaf nodes
        // Internal nodes (arange_64, range_64) have slots pointing to child nodes
        // Leaf nodes have slots pointing to actual data (vm_area_structs)

        let numSlots: number;
        let slotsOffset: number;
        let metadataOffset: number;

        if (mapleType === MAPLE_ARANGE_64) {
            // maple_arange_64 - ALWAYS an internal node!
            numSlots = 10;
            slotsOffset = 80;
            metadataOffset = 240;
            if (depth <= 1) {
//                 console.log(`${'  '.repeat(depth)}  Internal node (arange_64) - slots contain child node pointers`);
            }
        } else if (mapleType === MAPLE_RANGE_64) {
            // maple_range_64 - ALWAYS an internal node!
            numSlots = 16;
            slotsOffset = 128;
            metadataOffset = 256;
            if (depth <= 1) {
//                 console.log(`${'  '.repeat(depth)}  Internal node (range_64) - slots contain child node pointers`);
            }
        } else if (isLeafNode) {
            // Leaf node - contains BOTH pivots (keys) and values (vm_area_struct pointers)
            if (mapleType === MAPLE_DENSE) {
                // maple_dense stores values inline, typically 15-16 entries
                // Dense nodes don't have separate pivot array - values are packed
                numSlots = 15;
                slotsOffset = 8;  // Values start right after header
                metadataOffset = 248;
            } else {
                // maple_leaf_64 has complex structure:
                // - First part: pivot array (boundary addresses)
                // - Second part: slot array (actual VMA pointers)
                // - End: metadata
                numSlots = 16;  // Start with first 16, can expand if needed

                // In leaf_64, the layout is approximately:
                // [0x00-0x7F]: Pivots (16 x 8 bytes = 128 bytes)
                // [0x80-0xFF]: Slots (16 x 8 bytes = 128 bytes)
                // [0x100+]: Metadata
                slotsOffset = 128;  // Skip pivot array, start at slots
                metadataOffset = 256;
            }
            if (depth <= 1) {
//                 console.log(`${'  '.repeat(depth)}  Leaf node (${mapleType === MAPLE_DENSE ? 'dense' : 'leaf_64'}, type=0x${nodeType.toString(16)})`);
                if (mapleType === MAPLE_LEAF_64) {
//                     console.log(`${'  '.repeat(depth)}    Note: First 128 bytes are pivots (keys), next 128 bytes are slots (values)`);
                }
            }
        } else {
            // Unknown type - try to handle as potential data node
            numSlots = 16;
            slotsOffset = 128;
            metadataOffset = 256;
            if (depth <= 1) {
//                 console.log(`${'  '.repeat(depth)}  Unknown node type ${mapleType} (0x${nodeType.toString(16)})`);
            }
        }

        // Read metadata to get actual slot count
        const metadata = this.memory.readBytes(nodeOffset + metadataOffset, 16);
        let actualSlotCount = numSlots;
        if (metadata) {
            if (isLeafNode && depth <= 2) {
//                 console.log(`${'  '.repeat(depth)}  Metadata bytes: ${Array.from(metadata).map(b => `0x${b.toString(16).padStart(2, '0')}`).join(' ')}`);
            }
            // For leaf nodes, metadata layout is different
            // The first byte often indicates the number of valid entries
            if (metadata[0] > 0 && metadata[0] < numSlots) {
                actualSlotCount = metadata[0] + 1; // metadata[0] is often max_index
                if (depth <= 2) {
//                     console.log(`${'  '.repeat(depth)}  Metadata indicates ${actualSlotCount} valid slots`);
                }
            }
        }
        numSlots = actualSlotCount;

        // For leaf_64 nodes, also show the pivots (keys) AND the corresponding slots (values)
        if (isLeafNode && mapleType === MAPLE_LEAF_64 && depth <= 2) {
//             console.log(`${'  '.repeat(depth)}  Reading pivots (keys) and slots (values):`);
            for (let i = 0; i < Math.min(4, actualSlotCount); i++) {
                const pivot = this.memory.readU64(nodeOffset + (i * 8));  // Pivot at offset 0 + i*8
                const slot = this.memory.readU64(nodeOffset + 128 + (i * 8));  // Slot at offset 128 + i*8
                if (pivot || slot) {
//                     console.log(`${'  '.repeat(depth)}    [${i}] Pivot: 0x${pivot?.toString(16) || '0'} -> Slot: 0x${slot?.toString(16) || '0'}`);
                    // Check if this looks like a user VA boundary
                    if (pivot && pivot < 0x800000000000n) {
//                         console.log(`${'  '.repeat(depth)}      Pivot is user VA boundary`);
                    }
                    if (slot && (slot & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
//                         console.log(`${'  '.repeat(depth)}      Slot looks like kernel VA (potential VMA pointer)`);
                    }
                }
            }
        }

        // Process slots based on whether this is an internal or leaf node
        for (let i = 0; i < numSlots; i++) {
            const slotPtr = this.memory.readU64(nodeOffset + slotsOffset + (i * 8));
            if (!slotPtr || slotPtr === 0n) {
                continue;
            }

            // For maple dense nodes, check if we're looking at key-value pairs
            // In dense nodes for VMAs: even slots are keys (address ranges), odd slots are values (VMA pointers)
            if (isLeafNode && mapleType === MAPLE_DENSE) {
                if (i % 2 === 0 && i + 1 < numSlots) {
                    // This is a key (address range), next slot is the value (VMA pointer)
                    const nextSlot = this.memory.readU64(nodeOffset + slotsOffset + ((i + 1) * 8));
                    if (nextSlot && (nextSlot & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                        if (depth <= 2) {
//                             console.log(`${'  '.repeat(depth)}  Key-value pair: range_end=0x${slotPtr.toString(16)}, vma_ptr=0x${nextSlot.toString(16)}`);
                        }

                        // Try to extract VMA from the value (odd slot)
                        const translated = this.translateWithPgd(nextSlot, this.swapperPgDir);
                        if (translated) {
                            const vmaOffset = translated - Number(KernelConstants.GUEST_RAM_START);

                            // Debug: Check what's at the supposed VMA location
                            if (depth <= 2) {
                                const first64 = this.memory.readU64(vmaOffset);
                                const second64 = this.memory.readU64(vmaOffset + 8);
                                const third64 = this.memory.readU64(vmaOffset + 16);
//                                 console.log(`${'  '.repeat(depth)}    VMA@${nextSlot.toString(16)}: [0]=0x${first64?.toString(16)||'?'} [8]=0x${second64?.toString(16)||'?'} [16]=0x${third64?.toString(16)||'?'}`);
                            }

                            const vmStart = this.memory.readU64(vmaOffset);
                            const vmEnd = this.memory.readU64(vmaOffset + 8);

                            // More thorough validation
                            if (vmStart && vmEnd && vmEnd > vmStart &&
                                vmStart < 0x800000000000n && vmEnd < 0x800000000000n &&
                                (vmEnd - vmStart) >= 0x1000n) { // At least one page

                                const vmFlags = this.memory.readU64(vmaOffset + 32);

//                                 console.log(`${'  '.repeat(depth)}    ✓ FOUND VALID VMA: 0x${vmStart.toString(16)}-0x${vmEnd.toString(16)} (${((vmEnd - vmStart) / 1024n / 1024n)}MB, flags=0x${vmFlags?.toString(16) || '0'})`);

                                sections.push({
                                    startVa: VA(vmStart),
                                    endVa: VA(vmEnd),
                                    startPa: PA(0),
                                    size: Number(vmEnd - vmStart),
                                    pages: Math.ceil(Number(vmEnd - vmStart) / 4096),
                                    flags: BigInt(vmFlags || 0),
                                    type: 'data' as 'code' | 'data' | 'heap' | 'stack' | 'library' | 'kernel'
                                });

                                // Skip the next slot since we processed it as a value
                                continue;
                            }
                        }
                    }
                }
            }

            // Skip small values that are likely metadata/indices
            if (slotPtr < 0x1000n) {
                continue;
            }

            if (isInternalNode) {
                // INTERNAL NODE: Slots contain pointers to child nodes
                // These should be kernel VAs with encoding in low bits
                if ((slotPtr & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                    // This looks like a kernel VA - follow it as a child node
                    if (depth <= 2) {
                        const childType = Number(slotPtr & 0xFFn);
//                         console.log(`${'  '.repeat(depth)}  Slot[${i}]: Following child 0x${slotPtr.toString(16)} (type=0x${childType.toString(16)})`);
                    }
                    this.walkMapleTree(slotPtr, sections, depth + 1);
                }
            } else if (isLeafNode) {
                // LEAF NODE: Slots contain actual data (vm_area_structs)
                // These should be kernel VAs WITHOUT special encoding
                if (depth <= 2) {
//                     console.log(`${'  '.repeat(depth)}  Leaf slot[${i}]: 0x${slotPtr.toString(16)}`);

                    // Check if this could be ASCII text
                    const bytes: number[] = [];
                    for (let j = 0; j < 8; j++) {
                        const byte = Number((slotPtr >> BigInt(j * 8)) & 0xFFn);
                        bytes.push(byte);
                    }
                    const isPrintable = bytes.every(b => (b >= 32 && b <= 126) || b === 0);
                    if (isPrintable && bytes.some(b => b >= 32 && b <= 126)) {
                        const ascii = bytes.map(b => b >= 32 && b <= 126 ? String.fromCharCode(b) : '.').join('');
//                         console.log(`${'  '.repeat(depth)}    ASCII: "${ascii}"`);
                    }
                }

                if ((slotPtr & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                    // Try to read as vm_area_struct
                    const translated = this.translateWithPgd(slotPtr, this.swapperPgDir);
                    if (!translated) {
                        if (depth <= 2) {
//                             console.log(`${'  '.repeat(depth)}    Could not translate VA 0x${slotPtr.toString(16)}`);
                        }
                        continue;
                    }

                    const vmaOffset = translated - Number(KernelConstants.GUEST_RAM_START);
                    const vmStart = this.memory.readU64(vmaOffset);
                    const vmEnd = this.memory.readU64(vmaOffset + 8);
                    // Correct offsets from BTF/pahole:
                    const vmFlags = this.memory.readU64(vmaOffset + 0x20); // vm_flags at offset 0x20 (32 decimal)
                    const vmPgoff = this.memory.readU64(vmaOffset + 0x78); // vm_pgoff at offset 0x78 (120 decimal)
                    const vmFile = this.memory.readU64(vmaOffset + 0x80);  // vm_file at offset 0x80 (128 decimal)

                    if (depth <= 2 || sections.length < 5) {
//                         console.log(`${'  '.repeat(depth)}    Checking potential VMA at VA 0x${slotPtr.toString(16)} (PA 0x${translated.toString(16)})`);
//                         console.log(`${'  '.repeat(depth)}      vm_start=0x${vmStart?.toString(16) || '?'}, vm_end=0x${vmEnd?.toString(16) || '?'}`);
                        if (vmFile && vmFile !== 0n) {
//                             console.log(`${'  '.repeat(depth)}      vm_file=0x${vmFile.toString(16)} (has backing file)`);
                        }

                        // If these values look wrong, dump more fields to understand the struct
                        if (!vmStart || !vmEnd || vmStart === 0x1n || vmEnd === 0n || vmStart >= vmEnd) {
//                             console.log(`${'  '.repeat(depth)}      Invalid VMA values - dumping first 64 bytes:`);
                            for (let off = 0; off < 64; off += 8) {
                                const val = this.memory.readU64(vmaOffset + off);
                                if (val && val !== 0n) {
//                                     console.log(`${'  '.repeat(depth)}        [+0x${off.toString(16)}] = 0x${val.toString(16)}`);
                                }
                            }
                        }
                    }

                    // Validate it looks like a VMA (user addresses)
                    // Stricter validation: reject garbage values like vm_start=0x5
                    // ARM64 user space can go up to ~0xffffff000000 (48-bit addresses)
                    if (vmStart && vmEnd && vmEnd > vmStart &&
                        vmStart >= 0x10000n && vmStart < 0x1000000000000n &&  // User space limit for ARM64
                        vmEnd >= 0x10000n && vmEnd < 0x1000000000000n &&
                        (vmEnd - vmStart) >= 0x1000n) {  // At least one page

                        // Try to extract filename if vm_file exists
                        let filename: string | undefined;
                        if (vmFile && vmFile !== 0n && (vmFile & 0xFFFF000000000000n) === 0xFFFF000000000000n) {

                            // vm_file points to a struct file
                            const fileTranslated = this.translateWithPgd(vmFile, this.swapperPgDir);
                            if (fileTranslated) {
                                const fileOffset = fileTranslated - Number(KernelConstants.GUEST_RAM_START);

                                // Only log for debugging specific issues
                                const debugFileStruct = false;
                                if (debugFileStruct && depth <= 2) {
//                                     console.log(`${'  '.repeat(depth)}        File struct at PA 0x${fileTranslated.toString(16)}`);
                                }

                                // Correct offsets from BTF:
                                // struct file: f_path at offset 0x40 (64 decimal)
                                // struct path contains: mnt at +0, dentry at +8
                                // So dentry is at file offset 0x48
                                const dentry = this.memory.readU64(fileOffset + 0x48);

                                if (dentry && (dentry & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                                    const dentryTranslated = this.translateWithPgd(dentry, this.swapperPgDir);
                                    if (dentryTranslated) {
                                        const dentryOffset = dentryTranslated - Number(KernelConstants.GUEST_RAM_START);

                                        // dentry has d_name (qstr) at offset 0x20 (32 decimal)
                                        // qstr.name pointer is at offset 0x8 within qstr
                                        const dNamePtr = this.memory.readU64(dentryOffset + 0x20 + 0x8);

                                        if (dNamePtr) {

                                            if ((dNamePtr & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                                                const nameTranslated = this.translateWithPgd(dNamePtr, this.swapperPgDir);
                                                if (nameTranslated) {
                                                    const nameOffset = nameTranslated - Number(KernelConstants.GUEST_RAM_START);
                                                    // Try to read up to 256 bytes for the filename
                                                    const nameBytes = this.memory.readBytes(nameOffset, 256);
                                                    if (nameBytes) {
                                                        // Find null terminator
                                                        let nameLen = nameBytes.findIndex(b => b === 0);
                                                        if (nameLen === -1) nameLen = 256;
                                                        if (nameLen > 0) {
                                                            filename = new TextDecoder().decode(nameBytes.slice(0, nameLen));
                                                        }
                                                    }
                                                }
                                            } else {
                                                // Sometimes the name pointer might be embedded directly (short names)
                                                // Check if it looks like ASCII characters
                                                const bytes = [];
                                                for (let i = 0; i < 8; i++) {
                                                    const byte = Number((dNamePtr >> BigInt(i * 8)) & 0xFFn);
                                                    if (byte === 0) break;
                                                    if (byte >= 32 && byte <= 126) {
                                                        bytes.push(byte);
                                                    } else {
                                                        break;
                                                    }
                                                }
                                                if (bytes.length > 0) {
                                                    filename = String.fromCharCode(...bytes);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (sections.length < 10) {
                            let desc = `✓ Found valid VMA at depth ${depth}: 0x${vmStart.toString(16)}-0x${vmEnd.toString(16)} (flags=0x${vmFlags?.toString(16) || '0'})`;
                            if (filename) {
                                desc += ` [${filename}]`;
                            } else if (vmFile && vmFile !== 0n) {
                                desc += ` [file@0x${vmFile.toString(16)}]`;
                            }
//                             console.log(`${'  '.repeat(depth)}  ${desc}`);
                        }

                        // Determine type based on flags and filename
                        let type: 'code' | 'data' | 'heap' | 'stack' | 'library' | 'kernel' = 'data';
                        const flagBits = Number(vmFlags || 0);
                        const isExec = (flagBits & 0x4) !== 0;  // VM_EXEC
                        const isWrite = (flagBits & 0x2) !== 0; // VM_WRITE

                        if (filename) {
                            if (filename.includes('.so') || filename.includes('.dll')) {
                                type = 'library';
                            } else if (isExec) {
                                type = 'code';
                            }
                        } else if (!vmFile || vmFile === 0n) {
                            // Anonymous mapping - could be heap or stack
                            // Stack typically has high addresses and is RW
                            if (vmStart > 0x700000000000n) {
                                type = 'stack';
                            } else if (isWrite && !isExec) {
                                type = 'heap';
                            }
                        }

                        const section = {
                            startVa: Number(vmStart),
                            endVa: Number(vmEnd),
                            startPa: 0,
                            size: Number(vmEnd - vmStart),
                            pages: Math.ceil(Number(vmEnd - vmStart) / 4096),
                            flags: Number(vmFlags || 0),
                            type: type,
                            filename: filename,
                            fileOffset: vmFile ? Number(vmPgoff || 0) : undefined
                        };

                        // Debug: Log only sections with filenames
                        if (filename && sections.length < 5) {
//                             console.log(`VMA with filename: "${filename}" at 0x${vmStart.toString(16)}`);
                        }

                        sections.push(section);
                    } else if (depth <= 2) {
                        // In leaf nodes, invalid values are just garbage, don't follow them
//                         console.log(`${'  '.repeat(depth)}      Invalid VMA values (vm_start=0x${vmStart?.toString(16)||'?'}, vm_end=0x${vmEnd?.toString(16)||'?'}) - not following as child`);
                    }
                } else if (depth <= 2) {
                    // Not a kernel VA - could be other data
//                     console.log(`${'  '.repeat(depth)}    Slot value 0x${slotPtr.toString(16)} is not a kernel VA`);

                    // Check if it could be a physical address in guest RAM range
                    if (slotPtr >= BigInt(KernelConstants.GUEST_RAM_START) && slotPtr < BigInt(KernelConstants.GUEST_RAM_END)) {
//                         console.log(`${'  '.repeat(depth)}      -> Could be PA in guest RAM`);
                        // Try reading it as if it's a direct physical address
                        const directOffset = Number(slotPtr) - Number(KernelConstants.GUEST_RAM_START);
                        const testVmStart = this.memory.readU64(directOffset);
                        const testVmEnd = this.memory.readU64(directOffset + 8);
                        if (testVmStart && testVmEnd) {
//                             console.log(`${'  '.repeat(depth)}      -> As PA: vm_start=0x${testVmStart.toString(16)}, vm_end=0x${testVmEnd.toString(16)}`);
                        }
                    }

                    // Check if the high bits might be flags/metadata
                    const maskedPtr = slotPtr & 0x0000FFFFFFFFFFFFn;
                    if (maskedPtr !== slotPtr) {
//                         console.log(`${'  '.repeat(depth)}      -> High bits: 0x${((slotPtr >> 48n) & 0xFFFFn).toString(16)}, masked: 0x${maskedPtr.toString(16)}`);
                    }

                    // Check if this could be a user-space virtual address (key in the maple tree)
                    // User space VAs are typically < 0x0000800000000000
                    if (slotPtr < 0x800000000000n && slotPtr > 0x10000n) {
//                         console.log(`${'  '.repeat(depth)}      -> Could be user VA (key): maps to range around 0x${slotPtr.toString(16)}`);
                        // The next slot or a related slot might contain the vm_area_struct pointer
                    }

                    // Special check for values starting with 0xe... or 0xd...
                    // These might be encoded addresses or special maple tree values
                    const highNibble = Number((slotPtr >> 60n) & 0xFn);
                    if (highNibble === 0xE || highNibble === 0xD) {
                        // Try masking off the high nibble
                        const cleanAddr = slotPtr & 0x0FFFFFFFFFFFFFFFn;
//                         console.log(`${'  '.repeat(depth)}      -> High nibble 0x${highNibble.toString(16)}, cleaned: 0x${cleanAddr.toString(16)}`);

                        // Check if cleaned address makes more sense
                        if (cleanAddr < 0x800000000000n) {
//                             console.log(`${'  '.repeat(depth)}         -> Cleaned addr could be user VA!`);
                        }
                    }
                }
            }
        }
    }

    /**
     * Walk VMAs (Virtual Memory Areas) for a process
     * Returns memory sections mapped by the process
     */
    private walkVMAs(process: ProcessInfo): MemorySection[] {
        const sections: MemorySection[] = [];

        if (!process.mmStruct || !this.swapperPgDir) {
            return sections;
        }

        // Translate mm_struct VA to PA using kernel PGD
        const mmPa = this.translateWithPgd(process.mmStruct, this.swapperPgDir);
        if (!mmPa) {
            // Silently fail - not all processes have accessible mm_structs
            return sections;
        }

        // Modern kernels use maple tree, not linked list
        // Based on actual inspection, the layout appears to be:
        // 0x38: mm_users (atomic_t)
        // 0x40: mm_mt (maple tree) starts here
        // 0x48: mm_mt.ma_root (with encoded type in low bits)
        // 0x68: pgd (actual page global directory pointer)
        const MAPLE_TREE_OFFSET = 0x40;  // maple tree starts at 0x40
        const mmStructOffset = mmPa - Number(KernelConstants.GUEST_RAM_START);

        // First, verify this looks like a valid mm_struct
        const pgd = this.memory.readU64(mmStructOffset + 0x68);  // mm_struct->pgd at 0x68

        // mm_users is an atomic_t at offset 0x74 (verified via pahole)
        // Also check the old incorrect offsets for debugging
        const mmUsers38 = this.memory.readU32(mmStructOffset + 0x38);
        const mmUsers74 = this.memory.readU32(mmStructOffset + 0x74);  // Correct offset!
        const mmCount = this.memory.readU32(mmStructOffset + 0x0);     // mm_count at 0x0
        const mmUsers = mmUsers74;  // Use the correct offset 0x74

        const mapleRoot = this.memory.readU64(mmStructOffset + 0x48); // maple tree root at 0x48

        // Check interesting processes including Firefox and VLC
        if (process.name.toLowerCase().includes('firefox') ||
            process.name.toLowerCase().includes('vlc') ||
            process.pid === 6969 ||  // Firefox
            process.pid === 4917 || process.pid === 9909 ||  // VLC PIDs
            process.pid === 7168) {  // Firefox pool thread
//             console.log(`\n=== Checking mm_struct for PID ${process.pid} (${process.name}) ===`);
//             console.log(`    mm_struct VA: 0x${process.mmStruct.toString(16)}`);
//             console.log(`    mm_struct PA: 0x${mmPa.toString(16)}`);
//             console.log(`    maple root @ offset 0x48 = 0x${mapleRoot?.toString(16) || '?'}`);
//             console.log(`    pgd @ offset 0x68 = 0x${pgd?.toString(16) || '?'}`);
//             console.log(`    mm_count @ offset 0x00 = ${mmCount || 0}`);
            console.log(`    mm_users @ offset 0x74 = ${mmUsers74 || 0} (CORRECT)`);
//             console.log(`    mm_users @ offset 0x38 = ${mmUsers38 || 0} (wrong offset)`);

            // Dump first few fields to understand structure (only for processes with active mm_users)
            if (mmUsers > 0) {
//                 console.log(`    ✓ Process has active mm_users, should have valid VMAs`);
                for (let i = 0; i < 128; i += 8) {
                    const val = this.memory.readU64(mmStructOffset + i);
                    if (val && val !== 0n) {
//                         console.log(`    [0x${i.toString(16)}] = 0x${val.toString(16)}`);
                    }
                }
            } else {
//                 console.log(`    ⚠️  mm_users = ${mmUsers}, mm_count = ${mmCount}`);
                if (mmUsers === 0 && mmCount > 0) {
//                     console.log(`    Process exiting but mm still valid (mm_count > 0)`);
                } else if (mmUsers === 0 && mmCount === 0) {
//                     console.log(`    Process completely freed - maple tree may be corrupted`);
                }
                // Continue anyway to see what we find
//                 console.log(`    Continuing maple tree walk...`);
            }
        }

        // Based on /proc/maps analysis: mm_users=0 doesn't mean VMAs are gone!
        // VMAs persist until mm_count hits 0, so we should try walking the maple tree anyway
        if (!mmUsers || mmUsers === 0) {
            if (process.pid < 2000 || process.name.includes('firefox') || process.name.includes('vlc')) {
//                 console.log(`  Note: PID ${process.pid} (${process.name}) has mm_users = 0 but may still have VMAs`);
            }
            // Don't skip - /proc/maps works even when mm_users=0
        }

        // The maple root is directly at offset 0x48 in mm_struct
        // No need to add another offset
        const maRootPtr = mapleRoot;
        if (!maRootPtr || maRootPtr === 0n) {
            return sections; // Empty tree
        }


        const debug = process.pid < 100;
        if (debug) {
//             console.log(`  Maple tree root: 0x${maRootPtr.toString(16)}`);
        }

        // Check for special maple tree states (from kernel source and /proc/maps trace)
        // Special values: MAS_ROOT=0x1, MAS_NONE=0x2, others are rare
        // The kernel can also store a single VMA directly without a tree node
        if (maRootPtr < 0x100n) {
            if (debug || process.name.includes('firefox') || process.name.includes('vlc')) {
                const states: {[key: number]: string} = {
                    0x0: 'NULL (empty tree)',
                    0x1: 'MAS_ROOT (special marker)',
                    0x2: 'MAS_NONE (no entry)',
                    0x3: 'MAS_PAUSE',
                    0x5: 'MAS_START (might have entries)',
                    0x9: 'MAS_STOP',
                    0x11: 'MAS_ACTIVE'
                };
//                 console.log(`  Special maple tree state: ${states[Number(maRootPtr)] || `unknown (0x${maRootPtr.toString(16)})`}`);
            }

            // MAS_START might actually have entries - let's not give up immediately
            if (maRootPtr === 0x5n) {
//                 console.log(`  WARNING: MAS_START state but tree might have entries - kernel may be initializing`);
            }

            return sections; // No tree to walk for these special states
        }

        // Check if this is a direct entry (no encoding in low bits)
        // Entries with low 2 bits = 0b10 are reserved
        const lowBits = Number(maRootPtr & 3n);
        if (lowBits === 0) {
            // Could be a direct VMA pointer for single-entry tree
            // Try to read it as a vm_area_struct
            if ((maRootPtr & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                const translated = this.translateWithPgd(maRootPtr, this.swapperPgDir);
                if (translated) {
                    const vmaOffset = translated - Number(KernelConstants.GUEST_RAM_START);
                    const vmStart = this.memory.readU64(vmaOffset);
                    const vmEnd = this.memory.readU64(vmaOffset + 8);

                    if (vmStart && vmEnd && vmEnd > vmStart &&
                        vmStart < 0x800000000000n) {
                        if (debug) {
//                             console.log(`  Single VMA tree: 0x${vmStart.toString(16)}-0x${vmEnd.toString(16)}`);
                        }
                        sections.push({
                            startVa: Number(vmStart),
                            endVa: Number(vmEnd),
                            startPa: 0,
                            size: Number(vmEnd - vmStart),
                            pages: Math.ceil(Number(vmEnd - vmStart) / 4096),
                            flags: 0,
                            type: 'data' as 'code' | 'data' | 'heap' | 'stack' | 'library' | 'kernel'
                        });
                        return sections;
                    }
                }
            }
        }

        // Otherwise, it should be an encoded node pointer
        if (debug) {
            const nodeType = Number(maRootPtr & 0xFFn);
//             console.log(`  Maple tree root: 0x${maRootPtr.toString(16)} (type=0x${nodeType.toString(16)})`);
        }

        // Pass the encoded pointer - walkMapleTree will clean it
        this.walkMapleTree(maRootPtr, sections, 0);
        if (sections.length > 0 && debug) {
//             console.log(`  Found ${sections.length} VMAs via maple tree`);
        }
        return sections;
    }

    /* OLD LINKED LIST APPROACH (pre-6.1 kernels) - keeping for reference
    private walkVMAsOldLinkedList(process: ProcessInfo, mmPa: number): MemorySection[] {
        const sections: MemorySection[] = [];
        const vmaVa = this.memory.readU64(mmPa - Number(KernelConstants.GUEST_RAM_START) + KernelConstants.MM_MMAP);
        if (!vmaVa || vmaVa === 0n) {
            return sections;
        }

        const debug = process.pid < 100;
        if (debug) {
//             console.log(`  Walking VMAs for PID ${process.pid} (${process.name})`);
//             console.log(`    mm_struct at VA 0x${process.mmStruct.toString(16)} → PA 0x${mmPa.toString(16)}`);
//             console.log(`    First VMA at VA 0x${vmaVa.toString(16)}`);
        }

        // Walk the VMA linked list
        let currentVmaVa = vmaVa;
        let vmaCount = 0;
        const maxVmas = 100;  // Safety limit - most processes have < 50 VMAs
        const seenVmas = new Set<bigint>();  // Detect cycles

        while (currentVmaVa && currentVmaVa !== 0n && vmaCount < maxVmas) {
            // Check for cycles
            if (seenVmas.has(currentVmaVa)) {
//                 console.log(`VMA cycle detected at 0x${currentVmaVa.toString(16)}`);
                break;
            }
            seenVmas.add(currentVmaVa);

            // Check if VMA pointer is in kernel space
            if (currentVmaVa < 0xffff000000000000n) {
//                 console.log(`Invalid VMA pointer (not kernel VA): 0x${currentVmaVa.toString(16)}`);
                break;
            }

            // Translate VMA VA to PA
            const vmaPa = this.translateWithPgd(currentVmaVa, this.swapperPgDir);
            if (!vmaPa) {
                break;
            }

            const vmaOffset = vmaPa - Number(KernelConstants.GUEST_RAM_START);

            // Read VMA fields
            const vmStart = this.memory.readU64(vmaOffset + KernelConstants.VMA_VM_START);
            const vmEnd = this.memory.readU64(vmaOffset + KernelConstants.VMA_VM_END);
            const vmFlags = this.memory.readU64(vmaOffset + KernelConstants.VMA_VM_FLAGS);
            const vmNext = this.memory.readU64(vmaOffset + KernelConstants.VMA_VM_NEXT);

            // Sanity checks for valid VMAs
            if (!vmStart || !vmEnd || vmEnd <= vmStart) {
                break;
            }

            // Check for reasonable user-space addresses (not kernel)
            // User space is typically 0x0 to 0x0000800000000000 (128TB on ARM64)
            if (vmStart > 0x0000800000000000n || vmEnd > 0x0000800000000000n) {
//                 console.log(`Invalid VMA range: 0x${vmStart.toString(16)}-0x${vmEnd.toString(16)}`);
                break;
            }

            // Check for reasonable size (not bigger than 1TB)
            const size = vmEnd - vmStart;
            if (size > 0x10000000000n) { // 1TB
//                 console.log(`VMA too large: ${size} bytes`);
                break;
            }

            // Debug output for first VMA
            if (debug && vmaCount === 0) {
//                 console.log(`    First VMA details:`);
//                 console.log(`      Start: 0x${vmStart.toString(16)}`);
//                 console.log(`      End: 0x${vmEnd.toString(16)}`);
//                 console.log(`      Size: 0x${size.toString(16)} (${Number(size / 1024n)}KB)`);
//                 console.log(`      Flags: 0x${vmFlags.toString(16)}`);
//                 console.log(`      Next: 0x${vmNext.toString(16)}`);
            }

            // Skip if size is 0 or negative
            if (size <= 0) {
//                 console.log(`Invalid VMA size: ${size}`);
                break;
            }

            // Decode permissions from flags
            const perms = this.decodeVmaFlags(Number(vmFlags || 0));
            const sectionType = this.detectSectionType(Number(vmStart), Number(vmFlags || 0));

            sections.push({
                startVa: Number(vmStart),
                endVa: Number(vmEnd),
                startPa: 0,  // Will be filled in later if needed
                size: Number(size),
                pages: Math.ceil(Number(size) / 4096),
                flags: Number(vmFlags || 0),
                type: sectionType as 'code' | 'data' | 'heap' | 'stack' | 'library' | 'kernel'
            });

            currentVmaVa = (vmNext !== null && vmNext !== 0n) ? vmNext : 0n;
            vmaCount++;
        }

        return sections;
    }
    */

    /**
     * Decode VMA flags to permission string
     */
    private decodeVmaFlags(flags: number): string {
        let perms = '';
        perms += (flags & KernelConstants.VM_READ) ? 'r' : '-';
        perms += (flags & KernelConstants.VM_WRITE) ? 'w' : '-';
        perms += (flags & KernelConstants.VM_EXEC) ? 'x' : '-';
        perms += (flags & KernelConstants.VM_SHARED) ? 's' : 'p';
        return perms;
    }

    /**
     * Detect section type based on address and flags
     */
    private detectSectionType(address: number, flags: number): string {
        // Stack grows down
        if (flags & KernelConstants.VM_GROWSDOWN) {
            return 'stack';
        }

        // Heap grows up
        if (flags & KernelConstants.VM_GROWSUP) {
            return 'heap';
        }

        // Check address ranges
        if (address >= 0x400000 && address < 0x500000) {
            return 'code';
        }

        if (address >= 0x600000 && address < 0x700000) {
            return 'data';
        }

        // High addresses are usually stack
        if (address >= 0x7fff00000000) {
            return 'stack';
        }

        // Shared library range
        if (address >= 0x7f0000000000 && address < 0x800000000000) {
            return 'library';
        }

        // VDSO/VVAR
        if (address >= 0xffff00000000) {
            return 'vdso';
        }

        // Check permissions for hints
        if ((flags & KernelConstants.VM_EXEC) && !(flags & KernelConstants.VM_WRITE)) {
            return 'code';
        }

        if ((flags & KernelConstants.VM_WRITE) && !(flags & KernelConstants.VM_EXEC)) {
            return 'data';
        }

        return 'data';  // Default to data instead of anon which is not in the type
    }

    /**
     * Walk page tables to find all PTEs for a process
     */
    private walkProcessPageTables(process: ProcessInfo): PTE[] {
        const ptes: PTE[] = [];

        if (!process.pgd) {
            return ptes;
        }

        const debug = process.pid < 100;
        let validEntries = 0;

        // Walk all PGD entries - with ASLR, user space can use any index
        // Let's try walking all entries but log what we find
        // to understand the actual memory layout
        const debugProcess = process.name === 'vlc' || process.pid === 9526;
        if (debugProcess) {
//             console.log(`\nDEBUG: Walking page tables for VLC (PID ${process.pid})`);
        }

        for (let pgdIdx = 0; pgdIdx < 512; pgdIdx++) {
            // process.pgd is stored as physical address (with GUEST_RAM_START added)
            // We need to convert back to file offset
            const pgdPhysAddr = PhysicalAddress.add(process.pgd, pgdIdx * 8);
            const pgdOffset = Number(pgdPhysAddr) - Number(KernelConstants.GUEST_RAM_START);
            const pgdEntry = this.memory.readU64(pgdOffset);

            if (!pgdEntry || Number(pgdEntry & 3n) === 0) {
                continue;
            }

            validEntries++;
            if (debug && validEntries === 1) {
//                 console.log(`    First valid PGD entry at index ${pgdIdx}: 0x${pgdEntry.toString(16)}`);
            }

            // Walk through PUD, PMD, PTE levels
            this.walkPudLevel(pgdEntry, pgdIdx, ptes);
        }

        if (debug && validEntries === 0) {
//             console.log(`    No valid PGD entries found in user space`);
        }

        if (debugProcess) {
//             console.log(`    Total PTEs found: ${ptes.length}`);
            if (ptes.length > 0) {
                // Show sample VAs to understand the range
                const sampleVAs = ptes.slice(0, 5).map(p => `0x${p.va.toString(16)}`);
//                 console.log(`    Sample VAs: ${sampleVAs.join(', ')}`);
            }
        }

        return ptes;
    }

    /**
     * Walk kernel portion of page tables (indices 256-511)
     */
    private walkKernelPortionOfPageTables(process: ProcessInfo): PTE[] {
        const ptes: PTE[] = [];
        const debug = true;  // Enable for debugging
        let validEntries = 0;
        const nonZeroEntries: number[] = [];

        // Walk kernel space portion of PGD (indices 256-511)
        // Note: PGD[0] contains linear map but scanning it causes performance issues
        for (let pgdIdx = 256; pgdIdx < 512; pgdIdx++) {
            // process.pgd is stored as physical address (with GUEST_RAM_START added)
            // We need to convert back to file offset
            const pgdPhysAddr = PhysicalAddress.add(process.pgd, pgdIdx * 8);
            const pgdOffset = Number(pgdPhysAddr) - Number(KernelConstants.GUEST_RAM_START);
            const pgdEntry = this.memory.readU64(pgdOffset);

            if (!pgdEntry) {
                continue;
            }

            if (pgdEntry !== 0n) {
                nonZeroEntries.push(pgdIdx);
                if (debug && nonZeroEntries.length <= 5) {
//                     console.log(`      PGD[${pgdIdx}]: 0x${pgdEntry.toString(16)} (type=${Number(pgdEntry & 3n)})`);
                }
            }

            if (Number(pgdEntry & 3n) === 0) {
                continue;  // Entry not present
            }

            const entryType = Number(pgdEntry & 3n);

            if (entryType === 3) {  // Table descriptor - points to PUD
                validEntries++;
                if (debug && validEntries <= 3) {
//                     console.log(`    PGD[${pgdIdx}]: 0x${pgdEntry.toString(16)} -> PUD table`);
                }
                this.walkPudLevel(pgdEntry, pgdIdx, ptes);
            } else if (entryType === 1) {  // Block descriptor - 1GB page
                validEntries++;
                // Extract PA from bits [47:30] for 1GB pages at PGD level - mask off upper flag bits
                const pa = Number(pgdEntry & 0x0000FFFFC0000000n);
                const va = BigInt(pgdIdx) << 39n;  // PGD index determines bits [47:39]
                ptes.push({ va, pa, flags: pgdEntry, level: 1, pageSize: 1073741824 });
            }
        }

        if (debug || validEntries > 0) {
//             console.log(`    Walked kernel PGD indices 256-511:`);
//             console.log(`      Non-zero entries at indices: ${nonZeroEntries.join(', ')}`);
//             console.log(`      Valid entries (type 1 or 3): ${validEntries}`);
//             console.log(`      Total PTEs found: ${ptes.length}`);
        }

        return ptes;
    }

    /**
     * Walk PUD level of page tables
     */
    private walkPudLevel(pudTableAddr: bigint, pgdIdx: number, ptes: PTE[]): void {
        // Extract physical address from page table entry (bits [47:12])
        const pudBase = Number(pudTableAddr & 0x0000FFFFFFFFF000n);

        for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
            // Calculate file offset - high PAs need GUEST_RAM_START subtracted
            const pudPhysAddr = pudBase + (pudIdx * 8);
            const pudOffset = pudPhysAddr >= KernelConstants.GUEST_RAM_START
                ? pudPhysAddr - Number(KernelConstants.GUEST_RAM_START)
                : pudPhysAddr;
            const pudEntry = this.memory.readU64(pudOffset);

            if (!pudEntry || Number(pudEntry & 3n) === 0) {
                continue;
            }

            const entryType = Number(pudEntry & 3n);
            if (entryType === 1) {
                // 1GB huge page - use BigInt for proper 64-bit arithmetic
                const va = (BigInt(pgdIdx) << 39n) | (BigInt(pudIdx) << 30n);

                // Skip kernel VAs (those with bits 63-48 all set)
                if ((va >> 48n) === 0xFFFFn) {
                    continue;
                }

                const pudFlags = Number(pudEntry & 0xFFFn);
                // Extract PA from bits [47:30] for 1GB pages - mask off upper flag bits
                const pa = Number(pudEntry & 0x0000FFFFC0000000n);

                ptes.push({
                    va,  // Keep as BigInt
                    pa,
                    flags: pudFlags,
                    r: (pudFlags & 1) !== 0,
                    w: (pudFlags & 0x80) === 0,
                    x: (pudFlags & 0x10) === 0
                });
            } else {
                // Table descriptor, continue to PMD
                this.walkPmdLevel(pudEntry, pgdIdx, pudIdx, ptes);
            }
        }
    }

    /**
     * Walk PMD level of page tables
     */
    private walkPmdLevel(pmdTableAddr: bigint, pgdIdx: number, pudIdx: number, ptes: PTE[]): void {
        // Extract physical address from page table entry (bits [47:12])
        const pmdBase = Number(pmdTableAddr & 0x0000FFFFFFFFF000n);

        for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
            // Calculate file offset - high PAs need GUEST_RAM_START subtracted
            const pmdPhysAddr = pmdBase + (pmdIdx * 8);
            const pmdOffset = pmdPhysAddr >= KernelConstants.GUEST_RAM_START
                ? pmdPhysAddr - Number(KernelConstants.GUEST_RAM_START)
                : pmdPhysAddr;
            const pmdEntry = this.memory.readU64(pmdOffset);

            if (!pmdEntry || Number(pmdEntry & 3n) === 0) {
                continue;
            }

            const entryType = Number(pmdEntry & 3n);
            if (entryType === 1) {
                // 2MB huge page - use BigInt for proper 64-bit arithmetic
                const va = (BigInt(pgdIdx) << 39n) | (BigInt(pudIdx) << 30n) | (BigInt(pmdIdx) << 21n);

                // Skip kernel VAs (those with bits 63-48 all set)
                if ((va >> 48n) === 0xFFFFn) {
                    continue;
                }

                const pmdFlags = Number(pmdEntry & 0xFFFn);
                // Extract PA from bits [47:21] for 2MB pages - mask off upper flag bits
                const pa = Number(pmdEntry & 0x0000FFFFFFE00000n);

                ptes.push({
                    va,  // Keep as BigInt
                    pa,
                    flags: pmdFlags,
                    r: (pmdFlags & 1) !== 0,
                    w: (pmdFlags & 0x80) === 0,
                    x: (pmdFlags & 0x10) === 0
                });
            } else {
                // Table descriptor, continue to PTE
                this.walkPteLevel(pmdEntry, pgdIdx, pudIdx, pmdIdx, ptes);
            }
        }
    }

    /**
     * Walk PTE level of page tables
     */
    private walkPteLevel(pteTableAddr: bigint, pgdIdx: number, pudIdx: number, pmdIdx: number, ptes: PTE[]): void {
        // Extract physical address from page table entry (bits [47:12])
        const pteBase = Number(pteTableAddr & 0x0000FFFFFFFFF000n);

        for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
            // Calculate file offset - high PAs need GUEST_RAM_START subtracted
            const ptePhysAddr = pteBase + (pteIdx * 8);
            const pteOffset = ptePhysAddr >= KernelConstants.GUEST_RAM_START
                ? ptePhysAddr - Number(KernelConstants.GUEST_RAM_START)
                : ptePhysAddr;
            const pteEntry = this.memory.readU64(pteOffset);

            if (!pteEntry || Number(pteEntry & 3n) === 0) {
                continue;
            }

            // Regular 4KB page - use BigInt for proper 64-bit arithmetic
            const va = (BigInt(pgdIdx) << 39n) | (BigInt(pudIdx) << 30n) | (BigInt(pmdIdx) << 21n) | (BigInt(pteIdx) << 12n);

            // Debug suspicious VAs
            if (va > 0x0000FFFFFFFFFFFFn && va < 0xFFFF000000000000n) {
//                 console.log(`WARNING: Suspicious VA calculated: 0x${va.toString(16)}`);
//                 console.log(`  pgdIdx=${pgdIdx}, pudIdx=${pudIdx}, pmdIdx=${pmdIdx}, pteIdx=${pteIdx}`);
//                 console.log(`  Components: pgd<<39=0x${(BigInt(pgdIdx) << 39n).toString(16)}, pud<<30=0x${(BigInt(pudIdx) << 30n).toString(16)}, pmd<<21=0x${(BigInt(pmdIdx) << 21n).toString(16)}, pte<<12=0x${(pteIdx << 12).toString(16)}`);
            }

            // With modern ARM64 + ASLR, user processes can use high addresses
            // Only skip actual kernel space addresses (0xFFFF...)
            if ((va >> 48n) === 0xFFFFn) {
                continue;  // This is a kernel VA (bits 63-48 all set)
            }

            const pteFlags = Number(pteEntry & 0xFFFn);
            // Extract PA from bits [47:12] - mask off upper flag bits
            const pa = Number(pteEntry & 0x0000FFFFFFFFF000n);

            ptes.push({
                va,  // Keep as BigInt to preserve full 48-bit address
                pa,
                flags: pteFlags,
                r: (pteFlags & 1) !== 0,
                w: (pteFlags & 0x80) === 0,
                x: (pteFlags & 0x10) === 0
            });
        }
    }

    /**
     * Main discovery function
     */
    public async discover(totalSize: number): Promise<DiscoveryOutput> {
        console.time('Paged Kernel Discovery');
//         console.log(`Starting discovery on ${(totalSize / (1024*1024)).toFixed(0)}MB of memory`);
//         console.log(`Memory usage: ${this.memory.getMemoryUsage()}`);

        this.totalSize = totalSize;

        // Try to get kernel PGD from QMP ground truth first
        let kernelPgd = PA(0);
        try {
            // Check if we're in Electron and can use QMP via IPC
            if (typeof window !== 'undefined' && (window as any).electronAPI?.queryKernelInfo) {
                const result = await (window as any).electronAPI.queryKernelInfo();
                if (result.success && result.kernelPgd) {
                    kernelPgd = PA(result.kernelPgd);
                    console.log(`******** QMP: Got ground truth SWAPPER_PG_DIR = ${PhysicalAddress.toHex(kernelPgd).toUpperCase()} ********`);
                }
            } else if (typeof window === 'undefined') {
                // Node.js environment - can use direct connection
                const { queryKernelPGDViaQMP } = await import('./utils/qmp-client');
                const qmpResult = await queryKernelPGDViaQMP();
                if (qmpResult) {
                    kernelPgd = PA(qmpResult);
                    console.log(`******** QMP: Got ground truth SWAPPER_PG_DIR = ${PhysicalAddress.toHex(kernelPgd).toUpperCase()} ********`);
                }
            }
        } catch (err: any) {
            console.log(`QMP not available (${err.message}) - will use heuristic discovery`);
        }

        // If no QMP result, fall back to discovery
        if (kernelPgd === PA(0)) {
            kernelPgd = this.findSwapperPgDir();
            console.log(`Heuristic: Found kernel PGD = ${PhysicalAddress.toHex(kernelPgd)}`);
        }

        this.swapperPgDir = kernelPgd;
        this.kmem.setKernelPgd(Number(kernelPgd));

        // TEST: Page cache discovery early
        console.log('\n=== EARLY PAGE CACHE DISCOVERY TEST ===');
        console.log('Kernel PGD found at:', kernelPgd ? `0x${kernelPgd.toString(16)}` : 'not found');
        // Process discovery first - before page cache discovery
//         console.log('\n=== PROCESS DISCOVERY (ORIGINAL APPROACH) ===');
        this.findProcesses(totalSize);

        // Now do page cache discovery with processes already found
        const pageCacheDiscovery = new PageCacheDiscovery(this.memory, kernelPgd, undefined, this);
        const pageCacheResult = await pageCacheDiscovery.discover();
        console.log('Page cache discovery result:', pageCacheResult);

        // Prepare tasks array from discovered processes
        const tasks: Array<{pid: number, name: string, offset: number, mmPtr: bigint}> = [];
        let kernelThreadCount = 0;

        // Convert processes to tasks array for later use
        for (const [pid, process] of this.processes) {
            const mmPtr = process.mmStruct || 0n;
            if (mmPtr === 0n) {
                kernelThreadCount++;
            }
            tasks.push({pid, name: process.name, offset: 0, mmPtr});
        }

//         console.log(`  Found ${tasks.length} processes`);
//         console.log(`    - ${kernelThreadCount} kernel threads (no mm_struct)`);
//         console.log(`    - ${tasks.length - kernelThreadCount} user processes`);

        // SKIP the expensive PGD scan
        const SKIP_FULL_PGD_SCAN = true;
        let userPGDs: Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> = [];

        if (SKIP_FULL_PGD_SCAN) {
            // We'll populate userPGDs later after we categorize the PGDs from our scan
//             console.log('\n=== WILL USE MIXED PGDs FOR MATCHING ===');
//             console.log('(Mixed PGDs have both user and kernel entries - needed for vmalloc translation)');
        } else {
            // This code won't run unless we enable it
            const allPGDs = this.findAllPGDs();

            // Categorize PGDs by their characteristics
            // CORRECTED UNDERSTANDING: User process PGDs have BOTH user and kernel entries!
            const kernelPGDs = allPGDs.filter(p => p.kernelEntries >= 2 && p.userEntries <= 1);
            const pureUserPGDs = allPGDs.filter(p => p.kernelEntries === 0 && p.userEntries > 0); // Unusual
            userPGDs = allPGDs.filter(p => p.kernelEntries > 0 && p.userEntries > 1);  // REAL user PGDs

//         console.log(`\nPGD Analysis:`);
//         console.log(`  Total PGDs found: ${allPGDs.length}`);
//         console.log(`  Likely kernel PGDs: ${kernelPGDs.length}`);
//         console.log(`  User process PGDs (with kernel mappings): ${userPGDs.length}`);
//         console.log(`  Pure user PGDs (no kernel - unusual): ${pureUserPGDs.length}`);

        // Show kernel PGDs (should be just one - swapper_pg_dir)
        if (kernelPGDs.length > 0) {
//             console.log(`\nKernel PGDs:`);
            for (const pgd of kernelPGDs) {
//                 console.log(`  0x${pgd.addr.toString(16)}: ${pgd.kernelEntries} kernel, ${pgd.userEntries} user entries`);
                if (pgd.addr === 0x136dbf000) {
//                     console.log(`    ^ This is the known swapper_pg_dir!`);
                }
            }
        }

        // Show first few user PGDs
        if (userPGDs.length > 0) {
//             console.log(`\nFirst 10 user PGDs:`);
            for (let i = 0; i < Math.min(10, userPGDs.length); i++) {
                const pgd = userPGDs[i];
//                 console.log(`  0x${pgd.addr.toString(16)}: ${pgd.userEntries} user entries`);
            }
        }

        // Tasks array was already populated above from findProcesses()

        // Show first 20 tasks
//         console.log(`\nFirst 20 processes:`);
        for (let i = 0; i < Math.min(20, tasks.length); i++) {
            const t = tasks[i];
            const mmStr = t.mmPtr !== 0n ? `0x${t.mmPtr.toString(16)}` : 'NULL';
//             console.log(`  PID ${t.pid}: ${t.name} (mm=${mmStr})`);
        }

        // Show summary
//         console.log(`\n=== DISCOVERY SUMMARY ===`);
//         console.log(`Task analysis:`);
//         console.log(`  ${tasks.length} total processes`);
//         console.log(`  ${tasks.length - kernelThreadCount} user processes`);
//         console.log(`  ${kernelThreadCount} kernel threads`);
//         console.log(`\nPGD analysis:`);
//         console.log(`  ${userPGDs.length} user PGDs found`);
//         console.log(`  ${kernelPGDs.length} kernel PGDs found`);
//         console.log(`\nMatching challenge:`);
//         console.log(`  We have ${tasks.length - kernelThreadCount} user processes`);
//         console.log(`  But only ${userPGDs.length} user PGDs`);
        const diff = (tasks.length - kernelThreadCount) - userPGDs.length;
        if (diff > 0) {
//             console.log(`  ${diff} more processes than PGDs suggests:`);
//             console.log(`    - Processes sharing PGDs (threads of same process)`);
//             console.log(`    - False positives in task_struct detection`);
        } else if (diff < 0) {
//             console.log(`  ${-diff} more PGDs than processes suggests:`);
//             console.log(`    - Stale/unused PGDs still in memory`);
//             console.log(`    - Our task_struct detection missing some`);
        }
        } // Close the else block from SKIP_FULL_PGD_SCAN

        // Process list already populated by findProcesses() above
        // Skip early weak validation - kernel PGD already set in discover()
        // The QMP query was moved to the beginning of discover() to get ground truth first

        // Always run signature search to either confirm or as backup
        const signatureDiscoveredPgd = this.findSwapperPgdByAdaptiveSignature();

        // this.swapperPgDir was already set in discover() from QMP or heuristic
        if (this.swapperPgDir && signatureDiscoveredPgd) {
            // We have both - check if they match
            if (signatureDiscoveredPgd === this.swapperPgDir) {
//                 console.log(`  ✅ Signature search confirmed kernel PGD!`);
            } else {
//                 console.log(`  ⚠️ Signature search found different candidate: 0x${signatureDiscoveredPgd.toString(16)}`);
//                 console.log(`     Using already set value: 0x${this.swapperPgDir.toString(16)}`);
            }
            // Keep the already set value (from QMP or earlier discovery)
        } else if (!this.swapperPgDir && signatureDiscoveredPgd) {
            // No earlier result, but signature search found something
//             console.log(`  Using signature-discovered swapper_pg_dir at PA 0x${signatureDiscoveredPgd.toString(16)}`);
            this.swapperPgDir = signatureDiscoveredPgd;
            this.kmem.setKernelPgd(signatureDiscoveredPgd);
        } else if (!this.swapperPgDir) {
            // Nothing found - will try to discover below through validation
//             console.log('No kernel PGD found - will discover through validation');
        }

        // We'll validate discovered PGDs below

        /* Removed old hardcoded testing code - we now discover dynamically */

        // Configuration: Enable PGD scanning for forensics or when QMP unavailable
        const ENABLE_PGD_SCAN = false;  // Set to true for forensic analysis or QMP fallback

        // Initialize array even if scan is disabled
        const validationPGDs: Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> = [];

        if (ENABLE_PGD_SCAN) {
            // Enable exhaustive search to test our corrected understanding
//             console.log('\n=== SCANNING FOR ALL PGDs WITH CORRECTED UNDERSTANDING ===');
//             console.log('(User process PGDs should have BOTH user and kernel entries)');

        // Scan for all PGDs
        const pgdScanStart = 0x40000000;  // Start at 0GB PA (includes extracted PGDs at 0x04xxxxxxx)
        const pgdScanEnd = Math.min(totalSize, 0x180000000);  // Up to 6GB
        const pgdStride = 0x1000;  // 4KB aligned

//         console.log(`Scanning range: 0x${pgdScanStart.toString(16)} to 0x${pgdScanEnd.toString(16)}`);
        let candidateCount = 0;

        for (let pa = pgdScanStart; pa < pgdScanEnd; pa += pgdStride) {
            const offset = pa - Number(KernelConstants.GUEST_RAM_START);
            if (offset < 0 || offset >= totalSize) continue;

            const candidate = this.evaluatePgdCandidate(offset);
            if (candidate && candidate.score > 0) {
                validationPGDs.push(candidate);
                candidateCount++;

                // Log interesting candidates with both user and kernel entries
                if (candidate.userEntries > 0 && candidate.kernelEntries > 0) {
//                     console.log(`  Found mixed PGD at PA 0x${pa.toString(16)}: ${candidate.userEntries} user, ${candidate.kernelEntries} kernel entries`);
                }
            }
        }

//         console.log(`Found ${candidateCount} PGD candidates`);
        } else {
//             console.log('\n=== PGD SCAN DISABLED (set ENABLE_PGD_SCAN=true for forensics) ===');
        }

        // We'll discover the real kernel PGD below

        // Initialize variables outside the if block since they're used later
        let realKernelPGD = 0;
        const candidateResults: Array<{addr: number, score: number, validMmStructs: number, total: number}> = [];

        // Test ALL candidates to find the real kernel PGD with proper validation
        if (ENABLE_PGD_SCAN) {
//             console.log('\n=== TESTING ALL PGD CANDIDATES WITH STRICT VALIDATION ===');

        // Debug: Show what's in top PGD candidates
        if (validationPGDs.length > 0) {
//             console.log('\n=== ANALYZING TOP PGD CANDIDATES ===');
            for (let i = 0; i < Math.min(3, validationPGDs.length); i++) {
                const pgd = validationPGDs[i];
//                 console.log(`\nCandidate #${i+1} at 0x${pgd.addr.toString(16)}:`);

                // Read kernel space entries (indices 256-511 for kernel VAs starting with 0xffff)
                const kernelEntriesOffset = pgd.addr - Number(KernelConstants.GUEST_RAM_START) + (256 * 8);
                const kernelData = this.memory.readBytes(kernelEntriesOffset, 8 * 8); // Read 8 kernel entries

                if (kernelData) {
                    const view = new DataView(kernelData.buffer, kernelData.byteOffset, kernelData.byteLength);
//                     console.log('  Kernel space entries (for 0xffff... addresses):');
                    for (let j = 0; j < 8; j++) {
                        const entry = view.getBigUint64(j * 8, true);
                        if (entry !== 0n) {
                            const pa = Number(entry & ~0xFFFn);
                            const type = Number(entry & 3n);
//                             console.log(`    PGD[${256+j}]: 0x${entry.toString(16)} (PA: 0x${pa.toString(16)}, type: ${type})`);
                        }
                    }
                }
            }
        }

        for (let i = 0; i < validationPGDs.length; i++) {
            const candidate = validationPGDs[i];
            if (i < 10 || candidate.score >= 100) {
//                 console.log(`\nTesting candidate #${i+1}: 0x${candidate.addr.toString(16)} (score=${candidate.score})`);
            }

            // NEW VALIDATION: Count valid entries instead of translating mm_struct VAs
            const candidateOffset = candidate.addr - Number(KernelConstants.GUEST_RAM_START);
            const entryCount = this.testPgdWithEntryCountValidation(candidateOffset);

            candidateResults.push({
                addr: candidate.addr,
                score: candidate.score,
                validMmStructs: entryCount.validEntries,  // Reuse field name for valid entries
                total: entryCount.kernelEntries
            });

            // The REAL kernel PGD (swapper_pg_dir) characteristics:
            // - Very few user entries (typically 1-2, for kernel threads)
            // - A few kernel entries (typically 2-5, for essential kernel mappings)
            // - MUCH fewer total entries than user process PGDs (< 20 vs hundreds)
            // The key is: kernel doesn't need many mappings!
            if (entryCount.userEntries <= 5 &&
                entryCount.kernelEntries >= 2 && entryCount.kernelEntries <= 10 &&
                entryCount.validEntries <= 20) {
//                 console.log(`  ✓✓✓ POTENTIAL KERNEL PGD! Low entry count matches swapper_pg_dir pattern:`);
//                 console.log(`    - Total valid entries: ${entryCount.validEntries}`);
//                 console.log(`    - Kernel entries: ${entryCount.kernelEntries}`);
//                 console.log(`    - User entries: ${entryCount.userEntries}`);

                // Also verify with translation if possible
                const canTranslate = this.verifyKernelPgdByTranslation(candidate.addr - Number(KernelConstants.GUEST_RAM_START));
                if (canTranslate) {
//                     console.log(`    - ✓ Translation verification PASSED`);
                    realKernelPGD = candidate.addr;
                    if (!this.swapperPgDir) {
                        this.swapperPgDir = candidate.addr;
                        this.kmem.setKernelPgd(candidate.addr);
                    }
                    break; // Found it!
                } else {
//                     console.log(`    - ✗ Translation verification failed, but entry count still suggests kernel PGD`);
                    // Still might be kernel PGD even if translation fails
                    // (translation might fail due to different kernel configs)
                }

                // Analyze kernel PTE distribution to validate the structure
                this.analyzeKernelPTEDistribution();
                break;
            } else if (i < 10 || candidate.score >= 100) {
                if (entryCount.kernelEntries > 0) {
//                     console.log(`  → Partial validation: ${entryCount.kernelEntries} kernel entries (need ≥3 for analysis)`);
                } else {
//                     console.log(`  → No kernel entries found`);
                }
            }
        }

        // Report summary and categorize PGDs
//         console.log('\n=== PGD DISCOVERY RESULTS ===');
        const validCandidates = candidateResults.filter(c => c.validMmStructs > 0);
//         console.log(`Tested ${candidateResults.length} PGD candidates`);
//         console.log(`Found ${validCandidates.length} candidates with at least 1 valid entry`);

        // CORRECTED CATEGORIZATION: User process PGDs have BOTH user and kernel entries!
        const kernelOnlyPGDs = ENABLE_PGD_SCAN ? validationPGDs.filter(p => p.kernelEntries >= 2 && p.userEntries <= 1) : [];
        const mixedPGDs = ENABLE_PGD_SCAN ? validationPGDs.filter(p => p.kernelEntries > 0 && p.userEntries > 1) : [];  // REAL user process PGDs
        const userOnlyPGDs = ENABLE_PGD_SCAN ? validationPGDs.filter(p => p.kernelEntries === 0 && p.userEntries > 0) : []; // Should be rare/none

        // Populate userPGDs with the mixed PGDs for the matching code
        if (SKIP_FULL_PGD_SCAN) {
            userPGDs = mixedPGDs;
//             console.log(`\n=== NOW USING ${mixedPGDs.length} MIXED PGDs FOR MATCHING ===`);
        }

//         console.log('\n=== PGD CATEGORIZATION (Corrected Understanding) ===');
//         console.log(`  Kernel-only PGDs (swapper_pg_dir): ${kernelOnlyPGDs.length}`);
//         console.log(`  Mixed PGDs (user processes with kernel mappings): ${mixedPGDs.length}`);
//         console.log(`  User-only PGDs (unusual - should be none): ${userOnlyPGDs.length}`);

        if (kernelOnlyPGDs.length > 0) {
//             console.log('\nKernel PGDs (should be just swapper_pg_dir):');
            for (let i = 0; i < Math.min(3, kernelOnlyPGDs.length); i++) {
                const pgd = kernelOnlyPGDs[i];
                const pa = pgd.addr;
//                 console.log(`  PA 0x${pa.toString(16)}: ${pgd.kernelEntries} kernel, ${pgd.userEntries} user entries`);
            }
        }

        if (mixedPGDs.length > 0) {
//             console.log('\nUser Process PGDs (with shared kernel mappings):');
            for (let i = 0; i < Math.min(10, mixedPGDs.length); i++) {
                const pgd = mixedPGDs[i];
                const pa = pgd.addr;
//                 console.log(`  PA 0x${pa.toString(16)}: ${pgd.userEntries} user, ${pgd.kernelEntries} kernel entries`);
            }
            if (mixedPGDs.length > 10) {
//                 console.log(`  ... and ${mixedPGDs.length - 10} more`);
            }
        }

        if (userOnlyPGDs.length > 0) {
//             console.log('\nUnusual: User-only PGDs (no kernel mappings - investigate these):');
            for (const pgd of userOnlyPGDs.slice(0, 5)) {
                const pa = pgd.addr;
//                 console.log(`  PA 0x${pa.toString(16)}: ${pgd.userEntries} user entries only`);
            }
        }

        if (realKernelPGD) {
//             console.log(`\n✓ SUCCESS: Kernel PGD (swapper_pg_dir) confirmed at PA 0x${realKernelPGD.toString(16)}`);
        } else {
//             console.log(`\n⚠ WARNING: No PGD validated as kernel PGD with current criteria`);
        }
        } // End of ENABLE_PGD_SCAN block

        // Step 3: Match PIDs to PGDs using swapper_pg_dir (the kernel PGD)
//         console.log(`\n=== MATCHING PIDs TO PGDs ===`);
//         console.log(`Strategy: Use swapper_pg_dir to translate mm_struct addresses, then read PGD from mm_struct...`);

        const pidToPgd = new Map<number, number>();
        let matchCount = 0;
        let attemptCount = 0;
        let translateSuccess = 0;
        let pgdReadSuccess = 0;

        // Use the discovered kernel PGD (swapper_pg_dir) if we have it
        const kernelPGDForTranslation = this.swapperPgDir || realKernelPGD;

        if (!kernelPGDForTranslation) {
            console.log(`ERROR: No kernel PGD found - cannot translate mm_struct addresses`);
        } else {
//             console.log(`Using kernel PGD (swapper_pg_dir) at 0x${kernelPGDForTranslation.toString(16)} for translations`);

            // Check what PGD entries exist for vmalloc range
//             console.log('\n=== CHECKING KERNEL PGD FOR VMALLOC ENTRIES ===');
//             console.log('vmalloc range is 0xffff000000000000 - 0xffff7fffffffffff');

            // For ARM64 with 4KB pages and 48-bit VA:
            // PGD covers bits 47:39 (9 bits = 512 entries)
            // vmalloc at 0xffff0000... is in the kernel space (high canonical addresses)

//             console.log('Understanding ARM64 address space with ASLR:');
//             console.log('  0x0000000000000000 - 0x0000ffffffffffff : 48-bit user space');
//             console.log('    With ASLR: User processes use HIGH addresses (e.g., 0xc3..., 0xe7..., 0xf1...)');
//             console.log('    This maps to PGD indices throughout 0-511 (e.g., 371, 390, 482)');
//             console.log('  0xffff000000000000 - 0xffffffffffffffff : Kernel space (bits 63-48 all set)');

//             console.log('\nThe trick: vmalloc VA 0xffff000000000000 uses TTBR1 but maps to PGD[0]!');
//             console.log('This is because TTBR1 (kernel) sees the upper 256 entries as 0-255');

            // Read the kernel PGD to see what's mapped
            const pgdOffset = Number(kernelPGDForTranslation) - Number(KernelConstants.GUEST_RAM_START);
            const pgdData = this.memory.readBytes(pgdOffset, 512 * 8);

            if (pgdData) {
                const view = new DataView(pgdData.buffer, pgdData.byteOffset, pgdData.byteLength);
//                 console.log('Checking kernel PGD entries:');

                // Just show all non-zero entries
                const nonZero = [];
                for (let i = 0; i < 512; i++) {
                    const entry = view.getBigUint64(i * 8, true);
                    if (entry !== 0n) {
                        nonZero.push(i);
                    }
                }
//                 console.log(`  Non-zero entries at indices: ${nonZero.join(', ')}`);

                // Check PGD[0] specifically
                const pgd0 = view.getBigUint64(0, true);
                if (pgd0 !== 0n) {
//                     console.log(`\n  PGD[0] = 0x${pgd0.toString(16)} - THIS COVERS VMALLOC!`);
//                     console.log('  vmalloc addresses 0xffff0000... should go through PGD[0]');
//                     console.log('  But the translation still fails - why?');
                }
            }

            // For each process with an mm_struct pointer
            const userProcesses = Array.from(this.processes.values()).filter(p => p.mmStruct && p.mmStruct !== 0);
//             console.log(`Testing ${Math.min(20, userProcesses.length)} user processes...`);

            // First show a summary of all vmalloc addresses
//             console.log('\n=== VMALLOC ADDRESS SUMMARY ===');
//             console.log('All mm_struct addresses are in vmalloc space (0xffff0000... range):');
            for (const proc of userProcesses.slice(0, 10)) {
//                 console.log(`  PID ${proc.pid.toString().padStart(5)}: 0x${proc.mmStruct.toString(16)} (${proc.name})`);
            }
            if (userProcesses.length > 10) {
//                 console.log(`  ... and ${userProcesses.length - 10} more`);
            }
//             console.log('');

            for (const process of userProcesses.slice(0, 20)) { // Test first 20 processes
                const mmVA = process.mmStruct;
                // Strip PAC bits if present
                const mmClean = stripPAC(BigInt(mmVA));

                // Show the full VA and the cleaned VA after PAC stripping
//                 console.log(`\nPID ${process.pid.toString().padStart(5)} (${process.name.padEnd(15)}): mm_struct VA = 0x${mmVA.toString(16)}`);
                if (mmClean !== BigInt(mmVA)) {
//                     console.log(`  PAC stripped: 0x${Number(mmClean).toString(16)}`);
                }

                // Show which vmalloc range this falls into
                const mmVANum = typeof mmVA === 'bigint' ? Number(mmVA) : mmVA;
                if (mmVANum >= 0xffff000000000000 && mmVANum < 0xffff800000000000) {
                    const offset = mmVANum - 0xffff000000000000;
//                     console.log(`  vmalloc range: 0xffff0000... + 0x${offset.toString(16)}`);
                }

                attemptCount++;

                // Use kernel PGD to translate this mm_struct VA
                // First try the debug version to see where it fails
                if (process.pid === 1736) { // Just debug the first one
//                     console.log('  DEBUG: Walking page table for VA 0x' + Number(mmClean).toString(16));
//                     console.log('  As BigInt: 0x' + mmClean.toString(16));
                    this.debugTranslateVA(mmClean, kernelPGDForTranslation);
                }

                // Pass as number since translateVA converts it anyway
                const mmPA = this.translateWithPgd(Number(mmClean), kernelPGDForTranslation);

                // Debug: show what we got
                if (process.pid === 1736) {
//                     console.log(`  translateVA returned: ${mmPA === null ? 'null' : '0x' + mmPA.toString(16)}`);
                }

                if (mmPA && mmPA > 0) {  // Just check it's a valid PA, not in guest RAM range
                    translateSuccess++;
//                     console.log(`  ✓ Translated mm_struct to PA 0x${mmPA.toString(16)}`);

                    // Note if it's outside the normal guest RAM range
                    if (mmPA < KernelConstants.GUEST_RAM_START) {
//                         console.log(`    Note: PA is below guest RAM start (0x${KernelConstants.GUEST_RAM_START.toString(16)})`);
                    }

                    // Read the PGD pointer from the mm_struct
                    // For PAs below GUEST_RAM_START, we need to handle them specially
                    // These are in a different memory region that we can't access through our memory file
                    if (mmPA < KernelConstants.GUEST_RAM_START) {
//                         console.log(`    ✗ Cannot read mm_struct - PA 0x${mmPA.toString(16)} is outside memory file range`);
//                         console.log(`    This confirms vmalloc allocations are in kernel-reserved memory!`);
                    } else {
                        const mmOffset = mmPA - Number(KernelConstants.GUEST_RAM_START);
                        const pgdInMm = this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM);

                        if (pgdInMm) {
                            // PGD is stored as a kernel VA in mm_struct - need to translate it
//                             console.log(`    PGD kernel VA from mm_struct: 0x${pgdInMm.toString(16)}`);

                            let pgdPA: number;
                            if ((pgdInMm & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                                // It's a kernel VA - translate it
                                const translated = this.translateWithPgd(pgdInMm, kernelPGDForTranslation);
                                if (!translated) {
                                    console.log(`    ✗ Failed to translate PGD kernel VA to PA`);
                                    continue;
                                }
                                pgdPA = translated;
//                                 console.log(`    ✓ Translated PGD: VA 0x${pgdInMm.toString(16)} → PA 0x${pgdPA.toString(16)}`);
                            } else if (pgdInMm >= BigInt(KernelConstants.GUEST_RAM_START) &&
                                      pgdInMm < BigInt(KernelConstants.GUEST_RAM_END)) {
                                // Already a physical address
                                pgdPA = Number(pgdInMm);
//                                 console.log(`    PGD already physical: 0x${pgdPA.toString(16)}`);
                            } else {
//                                 console.log(`    ✗ Invalid PGD value: 0x${pgdInMm.toString(16)}`);
                                continue;
                            }

                            pgdReadSuccess++;
//                             console.log(`    ✓ Got PGD PA: 0x${pgdPA.toString(16)}`);

                            // Check if this PGD is in our list of mixed PGDs
                            const foundPgd = userPGDs.find(p => p.addr === pgdPA);
                            if (foundPgd) {
//                                 console.log(`      ✓✓ CONFIRMED: PGD is in our mixed PGD list (${foundPgd.userEntries} user, ${foundPgd.kernelEntries} kernel entries)`);
                                pidToPgd.set(process.pid, pgdPA);
                                matchCount++;
                            } else {
//                                 console.log(`      ⚠ PGD not in our mixed PGD list - might be outside scan range`);
                                // Still record it
                                pidToPgd.set(process.pid, pgdPA);
                                matchCount++;
                            }
                        } else {
//                             console.log(`    ✗ Invalid PGD in mm_struct: 0x${pgdInMm?.toString(16) || 'null'}`);
                        }
                    }
                } else {
                    console.log(`  ✗ Failed to translate mm_struct VA using kernel PGD`);
                }
            }
        }

//         console.log(`\n=== MATCHING RESULTS ===`);
//         console.log(`Attempted: ${attemptCount} processes`);
//         console.log(`Translated mm_struct: ${translateSuccess}/${attemptCount} (${(translateSuccess*100/attemptCount).toFixed(1)}%)`);
//         console.log(`Read valid PGD: ${pgdReadSuccess}/${translateSuccess} (${translateSuccess > 0 ? (pgdReadSuccess*100/translateSuccess).toFixed(1) : 0}%)`);
//         console.log(`Matched to PGDs: ${matchCount}/${pgdReadSuccess} (${pgdReadSuccess > 0 ? (matchCount*100/pgdReadSuccess).toFixed(1) : 0}%)`);

        if (matchCount > 0) {
//             console.log(`\nMatched PIDs:`);
            for (const [pid, pgd] of pidToPgd) {
                const process = this.processes.get(pid)!;
                const foundPgd = userPGDs.find(p => p.addr === pgd);
                if (foundPgd) {
//                     console.log(`  PID ${pid} (${process.name}) → PGD at 0x${pgd.toString(16)} (${foundPgd.userEntries} user, ${foundPgd.kernelEntries} kernel)`);
                } else {
//                     console.log(`  PID ${pid} (${process.name}) → PGD at 0x${pgd.toString(16)} (not in scan range)`);
                }
            }
        } else {
//             console.log(`\nNo matches found. Possible reasons:`);
            if (translateSuccess === 0) {
//                 console.log(`  - Kernel PGD cannot translate vmalloc addresses (expected!)`);
//                 console.log(`  - vmalloc addresses need per-process or vmalloc-specific mappings`);
            } else if (pgdReadSuccess === 0) {
//                 console.log(`  - PGD offset in mm_struct might be different than ${KernelConstants.PGD_OFFSET_IN_MM}`);
            }
        }

        this.findPageTables(totalSize);

        // Extract memory sections and PTEs for each process
        console.log('\n=== EXTRACTING PROCESS MEMORY INFO ===');
        const ptesByPid = new Map<number, PTE[]>();
        const sectionsByPid = new Map<number, MemorySection[]>();
        const pageCollection = new PageCollection();

        // Statistics for PGD extraction
        let validPgdCount = 0;
        let invalidPgdCount = 0;
        let noMmStructCount = 0;
        let translateFailCount = 0;
        const invalidSamples: Array<{pid: number, name: string, value: string}> = [];
        const validSamples: Array<{pid: number, name: string, pgd: number}> = [];

        let processCount = 0;
        for (const process of this.processes.values()) {
            // First, try to get PGD from mm_struct
            if (process.mmStruct && this.swapperPgDir) {
                // Try simple translation first for kernel VAs
                let mmPa = this.simpleTranslateKernelVA(process.mmStruct);

                // Fall back to complex translation if simple didn't work
                if (!mmPa) {
                    mmPa = this.translateWithPgd(process.mmStruct, this.swapperPgDir);
                }

                if (mmPa) {
                    // Debug: dump memory around mm_struct to see what we're reading
                    if (processCount < 5 || (process.pid > 0 && process.pid < 1000)) {
//                         console.log(`\n  PID ${process.pid} (${process.name}):`);
//                         console.log(`    mm_struct VA: 0x${process.mmStruct.toString(16)}`);
//                         console.log(`    mm_struct PA from translation: 0x${mmPa.toString(16)}`);

                        // Check if the PA makes sense and determine file offset
                        if (mmPa < KernelConstants.GUEST_RAM_START) {
//                             console.log(`    PA 0x${mmPa.toString(16)} is below GUEST_RAM_START (0x${KernelConstants.GUEST_RAM_START.toString(16)})`);
//                             console.log(`    Will try direct file offset: 0x${mmPa.toString(16)}`);
                        } else if (mmPa >= KernelConstants.GUEST_RAM_END) {
//                             console.log(`    WARNING: PA 0x${mmPa.toString(16)} is above GUEST_RAM_END (0x${KernelConstants.GUEST_RAM_END.toString(16)})`);
                        } else {
//                             console.log(`    File offset: 0x${(mmPa - Number(KernelConstants.GUEST_RAM_START)).toString(16)}`);
                        }

                        // Only try to read if the PA is in valid range OR if it's below GUEST_RAM_START
                        let mmBytes = null;
                        let effectiveOffset = 0;

                        if (mmPa >= KernelConstants.GUEST_RAM_START && mmPa < KernelConstants.GUEST_RAM_END) {
                            // Normal case - subtract GUEST_RAM_START
                            effectiveOffset = mmPa - Number(KernelConstants.GUEST_RAM_START);
                            mmBytes = this.memory.readBytes(effectiveOffset, 512);
                        } else if (mmPa < KernelConstants.GUEST_RAM_START && mmPa >= 0) {
                            // Try using PA directly as file offset for low memory
//                             console.log(`    Attempting to read from direct offset 0x${mmPa.toString(16)}`);
                            effectiveOffset = mmPa;
                            try {
                                mmBytes = this.memory.readBytes(effectiveOffset, 512);
                                if (mmBytes) {
//                                     console.log(`    SUCCESS: Read from low memory at offset 0x${mmPa.toString(16)}`);
                                }
                            } catch (e) {
//                                 console.log(`    FAILED: Cannot read from offset 0x${mmPa.toString(16)}`);
                            }
                        } else {
//                             console.log(`    Cannot read mm_struct - PA out of range`);
                        }

                        // mmBytes available for further debugging if needed
                    }

                    // Read PGD using the appropriate offset
                    // For simple-translated addresses, use them directly as file offsets
                    let pgdPtr = null;
                    if (mmPa >= 0 && mmPa < this.totalSize) {
                        // Use PA directly as file offset (no GUEST_RAM_START adjustment)
                        try {
                            pgdPtr = this.memory.readU64(mmPa + KernelConstants.PGD_OFFSET_IN_MM);
                            if (pgdPtr && processCount < 5) {
//                                 console.log(`    Read PGD from PA 0x${mmPa.toString(16)}: 0x${pgdPtr.toString(16)}`);
                            }
                        } catch (e) {
                            if (processCount < 5) {
//                                 console.log(`    Cannot read PGD from PA offset 0x${mmPa.toString(16)}`);
                            }
                        }
                    } else {
                        if (processCount < 5 || (process.pid > 0 && process.pid < 1000)) {
//                             console.log(`    Cannot read PGD - mm_struct PA 0x${mmPa.toString(16)} is outside accessible range`);
                        }
                    }

                    // Check if it's a kernel VA that needs translation
                    let finalPgd = 0;
                    if (pgdPtr) {
                        // Is this a kernel virtual address?
                        if ((pgdPtr & 0xFFFF000000000000n) === 0xFFFF000000000000n) {
                            // Try simple translation first for kernel VAs
                            let translatedPgd = this.simpleTranslateKernelVA(pgdPtr);

                            // Fall back to complex translation if simple didn't work
                            if (!translatedPgd) {
                                translatedPgd = this.translateWithPgd(pgdPtr, this.swapperPgDir);
                            }
                            if (translatedPgd && translatedPgd >= 0 && translatedPgd < this.totalSize) {
                                // Simple translation gives us file offset, but page table walk expects physical address
                                // So add GUEST_RAM_START to convert file offset to physical address
                                finalPgd = translatedPgd + KernelConstants.GUEST_RAM_START;
                                if (processCount < 5 || (process.pid > 0 && process.pid < 1000)) {
//                                     console.log(`    PGD: VA 0x${pgdPtr.toString(16)} -> file offset 0x${translatedPgd.toString(16)} -> PA 0x${finalPgd.toString(16)} ✓`);
                                }
                            }
                        }
                        // Or is it already a physical address?
                        else if (Number(pgdPtr) >= 0 && Number(pgdPtr) < this.totalSize) {
                            finalPgd = Number(pgdPtr);
                            if (processCount < 5 || (process.pid > 0 && process.pid < 1000)) {
//                                 console.log(`    PGD: PA 0x${finalPgd.toString(16)} (direct) ✓`);
                            }
                        }
                    }

                    if (finalPgd) {
                        process.pgd = finalPgd;
                        validPgdCount++;
                        if (validSamples.length < 5) {
                            validSamples.push({pid: process.pid, name: process.name, pgd: process.pgd});
                        }
                    } else {
                        invalidPgdCount++;
                        if (invalidSamples.length < 10) {
                            invalidSamples.push({pid: process.pid, name: process.name, value: pgdPtr?.toString(16) || null});
                        }
                        if (processCount < 5 || (process.pid > 0 && process.pid < 1000)) {
//                             console.log(`    PGD value: 0x${pgdPtr?.toString(16) || 'null'} - INVALID`);
                        }
                    }
                } else {
                    translateFailCount++;
                    if (processCount < 3 || (process.pid > 0 && process.pid < 1000)) {
//                         console.log(`  PID ${process.pid} (${process.name}): Could not translate mm_struct VA 0x${process.mmStruct.toString(16)}`);
                    }
                }
            } else {
                noMmStructCount++;
                if (processCount < 3) {
//                     console.log(`  PID ${process.pid} (${process.name}): No mm_struct or swapper_pg_dir`);
                }
            }

            // Walk page tables to get PTEs first
            if (process.pgd) {
                const ptes = this.walkProcessPageTables(process);
                if (ptes.length > 0) {
                    ptesByPid.set(process.pid, ptes);

                    // Add PTEs to page collection
                    ptes.forEach(pte => {
                        pageCollection.addPTEMapping(
                            Number(process.pid),
                            process.name,
                            pte.va,
                            pte.pa,
                            pte.flags
                        );
                    });

                    if (processCount < 3) {
//                         console.log(`    Found ${ptes.length} PTEs`);
                    }
                } else if (processCount < 3) {
//                     console.log(`    No PTEs found`);
                }
            }

            // Try walking VMAs via maple tree
            const sections = this.walkVMAs(process);
            if (sections.length > 0) {
                sectionsByPid.set(Number(process.pid), sections);

                // Add sections to page collection
                sections.forEach(section => {
                    // For now, sections don't have direct PA mapping until we walk PTEs
                    // But we can still track the VA ranges
                    pageCollection.addSectionInfo(
                        Number(process.pid),
                        process.name,
                        section
                    );
                });

                if (processCount < 3) {
//                     console.log(`    Found ${sections.length} memory sections via maple tree`);
                }
            }

            processCount++;
        }

        // Walk the kernel page tables (swapper_pg_dir) for kernel PTEs
//         console.log(`\n=== WALKING KERNEL PAGE TABLES (swapper_pg_dir) ===`);
        if (this.swapperPgDir) {
//             console.log(`Kernel PGD at PA: 0x${this.swapperPgDir.toString(16)}`);

            // Create a pseudo-process for the kernel
            const kernelProcess: ProcessInfo = {
                pid: 0n,
                name: 'kernel',
                mmStruct: 0n,
                pgd: this.swapperPgDir,
                isKernel: true
            };

            // Walk kernel page tables - we want the kernel portion (indices 256-511)
            const kernelPtes = this.walkKernelPortionOfPageTables(kernelProcess);

            if (kernelPtes.length > 0) {
//                 console.log(`  Found ${kernelPtes.length} kernel PTEs`);

                // Add kernel PTEs to the collection
                kernelPtes.forEach(pte => {
                    pageCollection.addPTEMapping(
                        0, // PID 0 for kernel
                        'kernel',
                        pte.va,
                        pte.pa,
                        pte.flags
                    );
                });

                // Store kernel PTEs
                ptesByPid.set(0n, kernelPtes);

                // Sample some kernel PTEs
//                 console.log(`  Sample kernel PTEs (first 5):`);
                kernelPtes.slice(0, 5).forEach(pte => {
                    const vaHex = pte.va.toString(16).padStart(16, '0');
                    const paHex = pte.pa.toString(16).padStart(12, '0');
//                     console.log(`    VA 0x${vaHex} -> PA 0x${paHex} (flags: 0x${pte.flags.toString(16)})`);
                });
            } else {
//                 console.log(`  No kernel PTEs found (this shouldn't happen!)`);
            }
        } else {
            console.log(`  ERROR: swapper_pg_dir not found!`);
        }

        // Report PGD extraction statistics
//         console.log(`\n=== PGD EXTRACTION STATISTICS ===`);
//         console.log(`Total processes: ${processCount}`);
//         console.log(`  - Valid PGDs extracted: ${validPgdCount}`);
//         console.log(`  - Invalid PGD values: ${invalidPgdCount}`);
//         console.log(`  - Translation failures: ${translateFailCount}`);
//         console.log(`  - No mm_struct: ${noMmStructCount}`);

        if (validSamples.length > 0) {
//             console.log(`\nValid PGD samples:`);
            validSamples.forEach(s => {
//                 console.log(`  PID ${s.pid} (${s.name}): PGD=0x${s.pgd.toString(16)}`);
            });
        }

        if (invalidSamples.length > 0) {
//             console.log(`\nInvalid PGD samples:`);
            invalidSamples.forEach(s => {
                // Handle null values
                if (!s.value) {
//                     console.log(`  PID ${s.pid} (${s.name}): null`);
                    return;
                }

                // Try to decode as ASCII if it looks like text
                const val = BigInt('0x' + s.value);
                let asciiStr = '';
                if (val > 0x20202020n && val < 0x7f7f7f7f7f7f7f7fn) {
                    for (let i = 0; i < 8; i++) {
                        const byte = Number((val >> BigInt(i * 8)) & 0xFFn);
                        if (byte >= 0x20 && byte <= 0x7f) {
                            asciiStr += String.fromCharCode(byte);
                        } else {
                            asciiStr = '';
                            break;
                        }
                    }
                }
//                 console.log(`  PID ${s.pid} (${s.name}): 0x${s.value}${asciiStr ? ` (ASCII: "${asciiStr}")` : ''}`);
            });
        }

//         console.log(`\nProcessed ${processCount} processes:`);
//         console.log(`  - ${ptesByPid.size} processes with PTEs found`);

        // Report simple translation statistics
        if (this.simpleTranslationSuccesses > 0 || this.simpleTranslationFailures > 0) {
//             console.log(`\n=== SIMPLE TRANSLATION STATS ===`);
//             console.log(`  Successes: ${this.simpleTranslationSuccesses}`);
//             console.log(`  Failures: ${this.simpleTranslationFailures}`);
            if (this.simpleTranslationSuccesses > 0) {
                const successRate = (this.simpleTranslationSuccesses / (this.simpleTranslationSuccesses + this.simpleTranslationFailures) * 100).toFixed(1);
//                 console.log(`  Success rate: ${successRate}%`);
            }
        }

        // Get page collection statistics
        const pageStats = pageCollection.getStats();
//         console.log(`\n=== PAGE COLLECTION STATS ===`);
//         console.log(`  Total pages tracked: ${pageStats.totalPages}`);
//         console.log(`  Total mappings: ${pageStats.totalMappings}`);
//         console.log(`  Shared pages: ${pageStats.sharedPages}`);
//         console.log(`  Unique processes: ${pageStats.uniqueProcesses}`);
//         console.log(`  Avg mappings/page: ${pageStats.avgMappingsPerPage.toFixed(2)}`);

        // Show page type breakdown
//         console.log(`  Page types:`);
        for (const [type, count] of pageStats.byType) {
//             console.log(`    ${type}: ${count}`);
        }

        // Example: Show some shared pages
        const sharedPages = pageCollection.getSharedPages();
        if (sharedPages.length > 0) {
//             console.log(`\n  Sample shared pages (first 3):`);
            sharedPages.slice(0, 3).forEach(page => {
//                 console.log(`    PA 0x${page.pa.toString(16)}: shared by ${page.mappings.length} processes`);
                page.mappings.slice(0, 2).forEach(m => {
//                     console.log(`      - PID ${m.pid} (${m.processName}) at VA 0x${m.va.toString(16)}`);
                });
            });
        }

        console.timeEnd('Paged Kernel Discovery');

        // Compare with previously set kernel PGD (from QMP or heuristic)
        if (this.swapperPgDir !== PA(0) && realKernelPGD && Number(this.swapperPgDir) !== realKernelPGD) {
            // If they differ, the one we set earlier (especially from QMP) takes precedence
//             console.log(`\n⚠ VALIDATION MISMATCH:`);
//             console.log(`  Initial discovery: ${PhysicalAddress.toHex(this.swapperPgDir)}`);
//             console.log(`  Later validation: 0x${realKernelPGD.toString(16)}`);
//             console.log(`  Keeping initial value`);
        } else if (this.swapperPgDir !== PA(0) && realKernelPGD && Number(this.swapperPgDir) === realKernelPGD) {
//             console.log('\n✓✓✓ VALIDATION SUCCESS: Kernel PGD confirmed!');
        }

        // FINAL SUMMARY - Concise output that won't get truncated
//         console.log('\n' + '='.repeat(60));
//         console.log('KERNEL DISCOVERY COMPLETE - FINAL SUMMARY');
//         console.log('='.repeat(60));

        // Get real processes (not garbage)
        const realProcesses = Array.from(this.processes.values()).filter(p => {
            // Must have a known name or valid kernel VA mm_struct
            const isKnown = KNOWN_PROCESSES.some(known => p.name.includes(known));
            const hasValidMm = p.mmStruct === 0n || p.mmStruct >= 0xffff000000000000n;
            return isKnown || (hasValidMm && p.name.match(/^[a-zA-Z\/][a-zA-Z0-9\-_\/\[\]:\.]*/));
        });

        const vlcProcesses = realProcesses.filter(p => p.name.toLowerCase().includes('vlc'));
        const knownProcesses = realProcesses.filter(p =>
            KNOWN_PROCESSES.some(known => p.name.includes(known)));

//         console.log(`✓ Found ${realProcesses.length} REAL processes (filtered from ${this.processes.size} candidates)`);
//         console.log(`  - Known system processes: ${knownProcesses.length}`);
        // Removed VLC special case

        if (this.swapperPgDir !== PA(0)) {
//             console.log(`\n✓ Kernel PGD (swapper_pg_dir): ${PhysicalAddress.toHex(this.swapperPgDir)}`);
        } else {
//             console.log(`\n✗ No valid kernel PGD found`);
        }

//         console.log('\nSample processes:');
        realProcesses.slice(0, 5).forEach(p => {
//             console.log(`  ${p.pid}: ${p.name} (kernel=${p.isKernelThread})`);
        });

//         console.log('='.repeat(60));

        return {
            processes: Array.from(this.processes.values()),
            ptesByPid,
            sectionsByPid,
            kernelPtes: this.kernelPtes,
            pageToPids: this.pageToPids,
            swapperPgDir: this.swapperPgDir,
            stats: this.stats,
            pageCollection,  // Return the page collection for UI use
        };
    }

    /**
     * Universal kernel PGD detection using VA translation verification
     * This works regardless of kernel version or memory layout
     */
    public verifyKernelPgdByTranslation(pgdOffset: number): boolean {
        // Key insight: Kernel PGD must be able to translate certain VAs
        // Test 1: Linear mapping region (most universal)
        const linearTestVAs = [
            0x1000,         // Should map to PA 0x40001000 (RAM_START + 0x1000)
            0x40000000,     // Should map to PA 0x80000000 (RAM_START + 0x40000000)
        ];

        const pgdPA = pgdOffset + KernelConstants.GUEST_RAM_START;

        for (const va of linearTestVAs) {
            const translatedPA = this.translateWithPgd(va, pgdPA);
            if (translatedPA) {
                // For linear mapping, VA offset should equal PA - RAM_START
                const expectedPA = KernelConstants.GUEST_RAM_START + va;
                if (translatedPA === expectedPA) {
//                     console.log(`  ✓ Linear mapping verified: VA 0x${va.toString(16)} → PA 0x${translatedPA.toString(16)}`);
                    return true; // This IS the kernel PGD!
                }
            }
        }

        // Test 2: Check if it has kernel space entries that lead to valid tables
        const pgdData = this.memory.readBytes(pgdOffset, 4096);
        if (!pgdData) return false;

        const view = new DataView(pgdData.buffer, pgdData.byteOffset, pgdData.byteLength);
        let kernelSpaceValid = 0;

        // Check upper half (kernel space)
        for (let i = 256; i < 512; i++) {
            const entry = view.getBigUint64(i * 8, true);
            if ((entry & 3n) === 3n) {
                // Follow this table and see if it's valid (mostly zeros)
                const tablePA = Number(entry & 0x0000FFFFFFFFF000n);
                const tableOffset = tablePA - Number(KernelConstants.GUEST_RAM_START);

                if (tableOffset > 0 && tableOffset < this.totalSize) {
                    const tableData = this.memory.readBytes(tableOffset, 512);
                    if (tableData) {
                        // Count zeros in the table
                        let zeros = 0;
                        const tableView = new DataView(tableData.buffer, tableData.byteOffset, tableData.byteLength);
                        for (let j = 0; j < 64; j++) {
                            if (tableView.getBigUint64(j * 8, true) === 0n) zeros++;
                        }
                        // Valid kernel tables are mostly zeros
                        if (zeros > 50) kernelSpaceValid++;
                    }
                }
            }
        }

        return kernelSpaceValid >= 1; // At least one valid kernel space entry
    }

    /**
     * Test a PGD candidate with entry counting validation (like Python)
     */
    private testPgdWithEntryCountValidation(pgdOffset: number): {validEntries: number, kernelEntries: number, userEntries: number} {
        const pgdAddr = pgdOffset + KernelConstants.GUEST_RAM_START;

        // Special debug for real kernel PGD
        const isRealPGD = pgdAddr === 0x136dbf000;
        if (isRealPGD) {
//             console.log(`    DEBUG: Testing real kernel PGD with entry counting validation`);
        }

        // Read the PGD page
        const pageBuffer = this.memory.readBytes(pgdOffset, KernelConstants.PAGE_SIZE);
        if (!pageBuffer) {
//             if (isRealPGD) console.log(`      Failed: Could not read PGD page`);
            return {validEntries: 0, kernelEntries: 0, userEntries: 0};
        }

        const view = new DataView(pageBuffer.buffer, pageBuffer.byteOffset, pageBuffer.byteLength);
        let validEntries = 0;
        let kernelEntries = 0;
        let userEntries = 0;

        // Count valid entries in all 512 PGD slots
        for (let i = 0; i < 512; i++) {
            const entryBig = view.getBigUint64(i * 8, true);

            // Check if this is a valid table descriptor (bits [1:0] = 0b11)
            const lowBits = Number(entryBig & 0x3n);
            if (lowBits !== 0x3) {
                continue;
            }

            // Extract physical address from bits [47:12]
            const physAddr = Number(entryBig & 0x0000FFFFFFFFF000n);

            // Validate physical address is reasonable (not clearly invalid)
            // Allow addresses outside GUEST_RAM since PUD/PMD/PTE tables may be in extended RAM
            if (physAddr < 0x1000 || physAddr > 0x200000000) { // Reject if below 4KB or above 8GB
                continue;
            }

            validEntries++;

            // Count kernel vs user entries
            if (i < 256) {
                userEntries++;
            } else {
                kernelEntries++;
            }

            if (isRealPGD && validEntries <= 10) {
//                 console.log(`      PGD[${i}]: entry=0x${entryBig.toString(16)}, physAddr=0x${physAddr.toString(16)} (${i < 256 ? 'user' : 'kernel'})`);
            }
        }

        if (isRealPGD) {
//             console.log(`      Total valid entries: ${validEntries} (${userEntries} user, ${kernelEntries} kernel)`);

            // Analyze kernel PGD entries in detail
//             console.log(`      Detailed kernel PGD entry analysis:`);
            for (let i = 256; i < 512; i++) {
                const entryBig = view.getBigUint64(i * 8, true);
                const lowBits = Number(entryBig & 0x3n);
                if (lowBits === 0x3) {
                    const physAddr = Number(entryBig & 0x0000FFFFFFFFF000n);
                    const vaBase = 0xffff000000000000n + (BigInt(i) << 39n);
//                     console.log(`        PGD[${i}]: VA range 0x${vaBase.toString(16)}-0x${(vaBase + (1n << 39n) - 1n).toString(16)} → PUD table at PA 0x${physAddr.toString(16)}`);
                }
            }
        }

        return {validEntries, kernelEntries, userEntries};
    }

    /**
     * Analyze kernel PTEs by virtual address ranges to validate PGD structure
     */
    private analyzeKernelPTEDistribution(): void {
//         console.log('\n=== KERNEL PTE DISTRIBUTION ANALYSIS ===');

        if (!this.swapperPgDir) {
//             console.log('No kernel PGD found, skipping analysis');
            return;
        }

        // Walk kernel page tables and collect PTEs by VA range
        const kernelPtes: Array<{va: bigint, pa: number, size: string}> = [];

        // Analyze ALL valid PGD entries (0-511) to find linear mapping
        for (let pgdIndex = 0; pgdIndex < 512; pgdIndex++) {
            const pgdOffset = this.swapperPgDir - Number(KernelConstants.GUEST_RAM_START) + (pgdIndex * 8);
            const pgdEntry = this.memory.readU64(pgdOffset);

            if (!pgdEntry || (pgdEntry & 3n) !== 3n) continue;

            // Calculate VA base for this PGD entry
            let vaBase: bigint;
            let regionType: string;

            if (pgdIndex < 256) {
                // User space or linear mapping
                vaBase = BigInt(pgdIndex) << 39n;
                regionType = pgdIndex === 0 ? "User/Linear" : "User";
            } else {
                // Kernel space
                vaBase = 0xffff000000000000n + (BigInt(pgdIndex - 256) << 39n);
                regionType = "Kernel";
            }

//             console.log(`\nAnalyzing PGD[${pgdIndex}] (${regionType}) VA range 0x${vaBase.toString(16)}-0x${(vaBase + (1n << 39n) - 1n).toString(16)}:`);

            // Walk this PGD entry's page tables
            const pgdPtes = this.walkKernelPageTablesForAnalysis(Number(pgdEntry & 0x0000FFFFFFFFF000n), vaBase, 1);
            // Use loop instead of spread operator to avoid stack overflow with large arrays
            for (const pte of pgdPtes) {
                kernelPtes.push(pte);
            }

            // Calculate total memory mapped in this region
            let totalMappedBytes = 0;
            let pteCount = 0;
            let blockCount2MB = 0;
            let blockCount1GB = 0;

            for (const pte of pgdPtes) {
                if (pte.size === '4KB') {
                    totalMappedBytes += 4 * 1024;
                    pteCount++;
                } else if (pte.size === '2MB') {
                    totalMappedBytes += 2 * 1024 * 1024;
                    blockCount2MB++;
                } else if (pte.size === '1GB') {
                    totalMappedBytes += 1024 * 1024 * 1024;
                    blockCount1GB++;
                }
            }

            const mappedMB = totalMappedBytes / (1024 * 1024);
            const mappedGB = mappedMB / 1024;

//             console.log(`  Found ${pgdPtes.length} total mappings in this 512GB region:`);
//             console.log(`    - ${pteCount} × 4KB PTEs = ${(pteCount * 4 / 1024).toFixed(1)}MB`);
//             console.log(`    - ${blockCount2MB} × 2MB blocks = ${(blockCount2MB * 2).toFixed(1)}MB`);
//             console.log(`    - ${blockCount1GB} × 1GB blocks = ${blockCount1GB}.0GB`);
//             console.log(`    Total mapped: ${mappedGB >= 1 ? mappedGB.toFixed(2) + 'GB' : mappedMB.toFixed(1) + 'MB'}`);
        }

        // Categorize PTEs by virtual address ranges
        const categories = {
            userLinear: { count: 0, range: '0x0000000000000000-0x0000007fffffffff', description: 'User space / Linear mapping of physical RAM' },
            userSpace: { count: 0, range: '0x0000008000000000-0x00007fffffffffff', description: 'User space' },
            kernelLinear: { count: 0, range: '0xffff000000000000-0xffff7fffffffffff', description: 'Kernel linear mapping' },
            vmalloc: { count: 0, range: '0xffff800000000000-0xffffbfffffffffff', description: 'vmalloc area' },
            modules: { count: 0, range: '0xffffc00000000000-0xfffffdffffffffff', description: 'Kernel modules' },
            fixmap: { count: 0, range: '0xfffffe0000000000-0xffffffffffffffff', description: 'Fixmap area' },
            other: { count: 0, range: 'other', description: 'Other areas' }
        };

        for (const pte of kernelPtes) {
            const va = pte.va;
            if (va >= 0x0000000000000000n && va < 0x0000008000000000n) {
                categories.userLinear.count++;
            } else if (va >= 0x0000008000000000n && va < 0x0000800000000000n) {
                categories.userSpace.count++;
            } else if (va >= 0xffff000000000000n && va < 0xffff800000000000n) {
                categories.kernelLinear.count++;
            } else if (va >= 0xffff800000000000n && va < 0xffffc00000000000n) {
                categories.vmalloc.count++;
            } else if (va >= 0xffffc00000000000n && va < 0xfffffe0000000000n) {
                categories.modules.count++;
            } else if (va >= 0xfffffe0000000000n) {
                categories.fixmap.count++;
            } else {
                categories.other.count++;
            }
        }

//         console.log(`\nTotal kernel PTEs found: ${kernelPtes.length}`);
//         console.log('Distribution by virtual address range:');
        for (const [name, cat] of Object.entries(categories)) {
            if (cat.count > 0) {
                const sizeMB = (cat.count * 4) / 1024; // 4KB pages to MB
//                 console.log(`  ${name}: ${cat.count} PTEs (${sizeMB.toFixed(1)}MB) - ${cat.description}`);
//                 console.log(`    Range: ${cat.range}`);
            }
        }

        // Calculate actual memory usage considering all page sizes
        let totalMappedBytes = 0;
        let totalPteCount = 0;
        let total2MBBlocks = 0;
        let total1GBBlocks = 0;

        for (const pte of kernelPtes) {
            if (pte.size === '4KB') {
                totalMappedBytes += 4 * 1024;
                totalPteCount++;
            } else if (pte.size === '2MB') {
                totalMappedBytes += 2 * 1024 * 1024;
                total2MBBlocks++;
            } else if (pte.size === '1GB') {
                totalMappedBytes += 1024 * 1024 * 1024;
                total1GBBlocks++;
            }
        }

        const totalMappedGB = totalMappedBytes / (1024 * 1024 * 1024);
        const theoreticalMaxGB = 3 * 512; // 3 PGD entries × 512GB each
        const utilizationPercent = (totalMappedGB / theoreticalMaxGB) * 100;

//         console.log(`\nKernel memory utilization summary:`);
//         console.log(`  Total mappings: ${kernelPtes.length} (${totalPteCount} × 4KB + ${total2MBBlocks} × 2MB + ${total1GBBlocks} × 1GB)`);
//         console.log(`  Actually mapped: ${totalMappedGB.toFixed(2)}GB`);
//         console.log(`  Theoretical max (3 × 512GB): ${theoreticalMaxGB}GB`);
//         console.log(`  Utilization: ${utilizationPercent.toFixed(4)}%`);
    }

    /**
     * Walk kernel page tables for PTE analysis - ITERATIVE version to prevent stack overflow
     */
    private walkKernelPageTablesForAnalysis(tableAddr: number, vaBase: bigint, level: number, depth: number = 0): Array<{va: bigint, pa: number, size: string}> {
        const ptes: Array<{va: bigint, pa: number, size: string}> = [];

        // Use a queue for iterative processing instead of recursion
        const queue: Array<{tableAddr: number, vaBase: bigint, level: number}> = [];
        queue.push({tableAddr, vaBase, level});

        let processedTables = 0;
        let totalGarbageEntries = 0;
        let block1GBCount = 0;
        let block2MBCount = 0;
        const visitedTables = new Set<number>(); // Cycle detection

        while (queue.length > 0) {
            const {tableAddr: currentAddr, vaBase: currentVA, level: currentLevel} = queue.shift()!;

            // Aggressive limits to prevent browser crash
            if (processedTables > 1000) {
//                 console.log(`    ⚠️  Processed ${processedTables} tables, stopping to prevent browser crash`);
                break;
            }

            if (queue.length > 5000) {
//                 console.log(`    ⚠️  Queue size ${queue.length} too large, stopping to prevent memory exhaustion`);
                break;
            }

            // Cycle detection
            if (visitedTables.has(currentAddr)) {
//                 console.log(`    🔄 Cycle detected at table 0x${currentAddr.toString(16)}, skipping`);
                continue;
            }
            visitedTables.add(currentAddr);

            // Validate table address is within guest RAM
            if (currentAddr < KernelConstants.GUEST_RAM_START || currentAddr >= KernelConstants.GUEST_RAM_END) {
                continue;
            }

            const offset = currentAddr - Number(KernelConstants.GUEST_RAM_START);
            let validEntries = 0;
            let garbageEntries = 0;
            processedTables++;

            for (let i = 0; i < 512; i++) {
                const entry = this.memory.readU64(offset + i * 8);
                if (!entry || entry === 0n) continue;

                // Check entry type in bits [1:0]
                const entryType = Number(entry & 3n);
                if (entryType === 0) continue; // Invalid

                // Extract physical address from bits [47:12]
                const physAddr = Number(entry & 0x0000FFFFFFFFF000n);

                // GARBAGE FILTERING: Validate physical address is reasonable
                if (physAddr < KernelConstants.GUEST_RAM_START || physAddr >= KernelConstants.GUEST_RAM_END) {
                    garbageEntries++;
                    continue; // Skip garbage entries that point outside guest RAM
                }

                // Check for obviously bogus addresses (too high or misaligned)
                if (physAddr % 4096 !== 0) {
                    garbageEntries++;
                    continue; // Physical addresses must be page-aligned
                }

                validEntries++;

                // Calculate virtual address for this entry
                const indexShift = [39, 30, 21, 12][currentLevel]; // PGD, PUD, PMD, PTE shifts
                const va = currentVA + (BigInt(i) << BigInt(indexShift));

                // CRITICAL: Handle level 3 (PTE) entries differently!
                if (currentLevel === 3) {
                    // At PTE level, type=3 means VALID PAGE, not table!
                    if (entryType === 3) {
                        // This is a valid 4KB page mapping
                        ptes.push({ va, pa: physAddr, size: '4KB' });
                    }
                    // Do NOT follow any PTE entries - we're at the bottom level!
                    // Types 0, 1, 2 at PTE level are invalid/reserved
                } else if (entryType === 1) {
                    // BLOCK DESCRIPTOR at PUD (1GB) or PMD (2MB) level
                    const blockSize = currentLevel === 1 ? '1GB' : '2MB';
                    if (currentLevel === 1) block1GBCount++;
                    else if (currentLevel === 2) block2MBCount++;
                    ptes.push({ va, pa: physAddr, size: blockSize });
                    // Do NOT follow - block descriptors point to actual memory
                } else if (entryType === 3 && currentLevel < 3) {
                    // TABLE DESCRIPTOR at PGD/PUD/PMD - Points to next level
                    // ONLY follow if we're not at PTE level!
                    queue.push({tableAddr: physAddr, vaBase: va, level: currentLevel + 1});
                }
            }

            totalGarbageEntries += garbageEntries;

            // Progress reporting every 100 tables
            if (processedTables % 100 === 0) {
//                 console.log(`    📊 Processed ${processedTables} tables, found ${ptes.length} PTEs so far, queue size: ${queue.length}`);
            }
        }

        if (totalGarbageEntries > 0 || block1GBCount > 0 || block2MBCount > 0) {
//             console.log(`    📊 Total: processed ${processedTables} tables, filtered ${totalGarbageEntries} garbage entries`);
//             console.log(`    📊 Block mappings: ${block1GBCount} × 1GB blocks, ${block2MBCount} × 2MB blocks`);
        }

        return ptes;
    }

    /**
     * Find kernel PGD using universal characteristics
     */
    public findKernelPgdUniversal(): number | null {
//         console.log('=== UNIVERSAL KERNEL PGD DISCOVERY ===');
//         console.log('Using VA translation verification (works on any kernel)...');

        const candidates: Array<{offset: number, reason: string}> = [];
        let scanned = 0;
        let testedTranslations = 0;

        // Scan memory for pages that look like PGDs
        // Focus on areas where kernel structures typically reside
        const scanRanges = [
            // Scan backwards from end (kernel structures often at high addresses)
            { start: Math.max(0, this.totalSize - 1024*1024*1024), end: this.totalSize },
            // Also check 2-4GB range
            { start: 2*1024*1024*1024, end: Math.min(4*1024*1024*1024, this.totalSize) },
        ];

        for (const range of scanRanges) {
//             console.log(`  Scanning ${(range.start/(1024*1024*1024)).toFixed(1)}GB - ${(range.end/(1024*1024*1024)).toFixed(1)}GB...`);

            for (let offset = range.start; offset < range.end; offset += 4096) {
                if (offset < 0 || offset >= this.totalSize) continue;
                scanned++;

                // Quick pre-filter: Must have sparse entries
                const preview = this.memory.readBytes(offset, 256);
                if (!preview) continue;

                const view = new DataView(preview.buffer, preview.byteOffset, preview.byteLength);
                let validEntries = 0;
                let zeroEntries = 0;
                let hasEntry0 = false;

                for (let i = 0; i < 32; i++) {
                    const entry = view.getBigUint64(i * 8, true);
                    if (entry === 0n) {
                        zeroEntries++;
                    } else if ((entry & 3n) === 3n) {
                        validEntries++;
                        if (i === 0) hasEntry0 = true;
                    }
                }

                // Skip if not sparse enough or missing entry 0
                if (!hasEntry0 || validEntries === 0 || validEntries > 5 || zeroEntries < 25) {
                    continue;
                }

                // Now do the expensive translation test
                testedTranslations++;
                if (this.verifyKernelPgdByTranslation(offset)) {
                    candidates.push({
                        offset,
                        reason: 'Successfully translates linear mapping VAs'
                    });
//                     console.log(`  ✓ FOUND kernel PGD at offset 0x${offset.toString(16)} (PA: 0x${(offset + KernelConstants.GUEST_RAM_START).toString(16)})`);
//                     console.log(`    Reason: ${candidates[candidates.length - 1].reason}`);

                    // Validate with entry counting
                    const validation = this.testPgdWithEntryCountValidation(offset);
//                     console.log(`    Validation: ${validation.validEntries} entries (${validation.kernelEntries} kernel, ${validation.userEntries} user)`);

                    // Usually only one kernel PGD, so we can return early
                    return offset + KernelConstants.GUEST_RAM_START;
                }
            }
        }

//         console.log(`\nScanned ${scanned} pages, tested ${testedTranslations} translation candidates`);

        if (candidates.length === 0) {
//             console.log('No kernel PGD found using universal method!');
            return null;
        }

        return candidates[0].offset + KernelConstants.GUEST_RAM_START;
    }

    /**
     * Find ALL PGDs in the entire memory space (standalone, at bottom for easy finding)
     */
    public findAllPGDs(): Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> {
//         console.log('=== EXHAUSTIVE PGD SEARCH ===');
//         console.log(`Scanning entire ${(this.totalSize / (1024*1024)).toFixed(0)}MB memory space for PGDs...`);

        const pgdCandidates: Array<{addr: number, score: number, kernelEntries: number, userEntries: number}> = [];
        let pagesScanned = 0;
        let candidatesFound = 0;

        // Scan EVERY 4KB page in the entire memory space
        for (let offset = 0; offset < this.totalSize; offset += 4096) {
            pagesScanned++;

            // Progress update every 1GB
            if (offset > 0 && offset % (1024 * 1024 * 1024) === 0) {
//                 console.log(`  Scanned ${(offset / (1024*1024*1024)).toFixed(1)}GB: found ${candidatesFound} PGD candidates so far...`);
            }

            // Quick pre-check: Read first few entries to see if it could be a PGD
            const pageBuffer = this.memory.readBytes(offset, 64); // First 8 entries
            if (!pageBuffer) continue;

            const view = new DataView(pageBuffer.buffer, pageBuffer.byteOffset, pageBuffer.byteLength);

            // Check if any entries look like page table pointers
            // Check both user space (0-7) and kernel space (256-263)
            let possiblePTE = false;
            for (let i = 0; i < 8; i++) {
                // Use getBigUint64 to avoid precision issues
                const entryBig = view.getBigUint64(i * 8, true);
                const entryType = Number(entryBig & 0x3n);

                // Check if entry has valid bits (1 or 3)
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
//                     console.log(`  Found PGD at 0x${candidate.addr.toString(16)}: score=${candidate.score}, kernel=${candidate.kernelEntries}, user=${candidate.userEntries}`);
                }
            }
        }

//         console.log(`\n=== PGD SEARCH COMPLETE ===`);
//         console.log(`Scanned ${pagesScanned} pages (${(pagesScanned * 4 / 1024).toFixed(0)}MB)`);
//         console.log(`Found ${candidatesFound} PGD candidates total`);

        // Sort by score and show top 10
        pgdCandidates.sort((a, b) => b.score - a.score);
//         console.log(`\nTop 10 PGD candidates:`);
        for (let i = 0; i < Math.min(10, pgdCandidates.length); i++) {
            const c = pgdCandidates[i];
//             console.log(`  ${i+1}. 0x${c.addr.toString(16)}: score=${c.score}, kernel=${c.kernelEntries}, user=${c.userEntries}`);
        }

        return pgdCandidates;
    }
}