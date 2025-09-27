/**
 * Kernel Memory Helper
 * Simple module for reading kernel memory with VA translation
 */

import { PagedMemory } from './paged-memory';

// Import the types and constants we need
import type { ProcessInfo, PTE, MemorySection } from './kernel-discovery';
import { KernelConstants, MemoryConfig, stripPAC } from './kernel-discovery';
import { VirtualAddress, VA } from './types/virtual-address';
import { PhysicalAddress, PA } from './types/physical-address';

// Re-export types for use in other modules
export type { ProcessInfo, PTE, MemorySection };
export { KernelConstants };

// Known processes for validation
const KNOWN_PROCESSES = [
    'init', 'systemd', 'kthreadd', 'rcu_gp', 'migration',
    'ksoftirqd', 'kworker', 'kcompactd', 'khugepaged',
    'kswapd', 'kauditd', 'sshd', 'systemd-journal',
    'systemd-resolved', 'systemd-networkd', 'bash',
    'vlc', 'firefox', 'chrome', 'code', 'vim', 'emacs'
];

// Structure offsets from pahole
export const OFFSETS = {
    // task_struct
    'task.files': 0x990,  // Updated for Ubuntu kernel
    'task.comm': 0x758,
    'task.pid': 0x4E8,
    'task.mm': 0x998,
    'task.tasks': 0x508,

    // files_struct
    'files.fdt': 0x20,

    // fdtable
    'fdt.fd': 0x08,
    'fdt.max_fds': 0x00,

    // file
    'file.inode': 0x28,

    // inode
    'inode.ino': 0x40,
    'inode.size': 0x50,
    'inode.mode': 0x00,
};

export class KernelMem {
    private memory: PagedMemory;
    private kernelPgd: number = 0x82a12000; // Default swapper_pg_dir PA

    constructor(memory: PagedMemory) {
        this.memory = memory;
    }

    /**
     * Set kernel PGD physical address
     */
    setKernelPgd(pa: number) {
        this.kernelPgd = pa;
    }

    /**
     * Get current kernel PGD physical address
     */
    getKernelPgd(): number {
        return this.kernelPgd;
    }

    /**
     * Query kernel PGD from QMP ground truth
     * This should always be used when available as it's the authoritative source
     */
    async queryGroundTruthPgd(): Promise<number | null> {
        try {
            // Check if we're in Electron and can use QMP via IPC
            if (typeof window !== 'undefined' && (window as any).electronAPI?.queryKernelInfo) {
                const result = await (window as any).electronAPI.queryKernelInfo();
                if (result.success && result.kernelPgd) {
                    const kernelPgd = result.kernelPgd;
                    console.log(`******** QMP: Got ground truth SWAPPER_PG_DIR = 0x${kernelPgd.toString(16).toUpperCase()} ********`);
                    this.setKernelPgd(kernelPgd); // Automatically set it
                    return kernelPgd;
                }
            } else if (typeof window === 'undefined') {
                // Node.js environment - can use direct connection
                const { queryKernelPGDViaQMP } = await import('./utils/qmp-client');
                const qmpResult = await queryKernelPGDViaQMP();
                if (qmpResult) {
                    console.log(`******** QMP: Got ground truth SWAPPER_PG_DIR = 0x${qmpResult.toString(16).toUpperCase()} ********`);
                    this.setKernelPgd(qmpResult); // Automatically set it
                    return qmpResult;
                }
            }
        } catch (err: any) {
            console.log(`QMP not available (${err.message}) - will use fallback`);
        }
        return null;
    }

    /**
     * Translate kernel virtual address to physical
     */
    translateVA(va: bigint): number | null {
        // Page table walk for other addresses
        const pgdIndex = Number((va >> 39n) & 0x1FFn);
        const pudIndex = Number((va >> 30n) & 0x1FFn);
        const pmdIndex = Number((va >> 21n) & 0x1FFn);
        const pteIndex = Number((va >> 12n) & 0x1FFn);
        const offset = Number(va & 0xFFFn);

        // Debug for vmalloc addresses
        const isVmalloc = (va >= 0xffff000000000000n && va < 0xffff800000000000n);
        if (isVmalloc) {
            console.log(`  Translating vmalloc VA: 0x${va.toString(16)}`);
            console.log(`    PGD index: ${pgdIndex}, PUD: ${pudIndex}, PMD: ${pmdIndex}, PTE: ${pteIndex}`);
            console.log(`    Using kernel PGD: 0x${this.kernelPgd.toString(16)}`);
        }

        // Read PGD entry
        const pgdData = this.memory.readBytes((this.kernelPgd - 0x40000000) + pgdIndex * 8, 8);
        if (!pgdData) {
            if (isVmalloc) console.log(`    Failed to read PGD entry at PA: 0x${((this.kernelPgd - 0x40000000) + pgdIndex * 8).toString(16)}`);
            return null;
        }

        const pgdEntry = new DataView(pgdData.buffer, pgdData.byteOffset, pgdData.byteLength).getBigUint64(0, true);
        // Check valid bit (bit 0) - ARM64 page table entry
        if ((pgdEntry & 0x1n) === 0n) {
            if (isVmalloc) console.log(`    PGD entry not valid: 0x${pgdEntry.toString(16)}`);
            return null;
        }

        // Extract PA from bits 47:12 (ARM64 48-bit PA)
        const pudPA = Number(pgdEntry & 0x0000FFFFFFFFF000n);
        if (isVmalloc) console.log(`    PGD entry: 0x${pgdEntry.toString(16)}, extracted PUD PA: 0x${pudPA.toString(16)}`);

        // Read PUD entry
        const pudData = this.memory.readBytes((pudPA - 0x40000000) + pudIndex * 8, 8);
        if (!pudData) {
            if (isVmalloc) console.log(`    Failed to read PUD entry at PA: 0x${((pudPA - 0x40000000) + pudIndex * 8).toString(16)}`);
            return null;
        }

        const pudEntry = new DataView(pudData.buffer, pudData.byteOffset, pudData.byteLength).getBigUint64(0, true);
        if ((pudEntry & 0x1n) === 0n) {
            if (isVmalloc) console.log(`    PUD entry not valid: 0x${pudEntry.toString(16)}`);
            return null;  // Check valid bit
        }

        const pmdPA = Number(pudEntry & 0x0000FFFFFFFFF000n);

        // Read PMD entry
        const pmdData = this.memory.readBytes((pmdPA - 0x40000000) + pmdIndex * 8, 8);
        if (!pmdData) return null;

        const pmdEntry = new DataView(pmdData.buffer, pmdData.byteOffset, pmdData.byteLength).getBigUint64(0, true);
        if ((pmdEntry & 0x1n) === 0n) return null;  // Check valid bit

        const ptePA = Number(pmdEntry & 0x0000FFFFFFFFF000n);

        // Read PTE entry
        const pteData = this.memory.readBytes((ptePA - 0x40000000) + pteIndex * 8, 8);
        if (!pteData) return null;

        const pteEntry = new DataView(pteData.buffer, pteData.byteOffset, pteData.byteLength).getBigUint64(0, true);
        if ((pteEntry & 0x1n) === 0n) return null;  // Check valid bit

        const pagePA = Number(pteEntry & 0x0000FFFFFFFFF000n);
        return pagePA + offset;
    }

    /**
     * Read memory at virtual address
     */
    read(va: bigint, size: number): Uint8Array | null {
        const pa = this.translateVA(va);
        if (!pa) return null;
        return this.memory.readBytes(pa - 0x40000000, size);
    }

    /**
     * Read u32 at virtual address
     */
    readU32(va: bigint): number | null {
        const data = this.read(va, 4);
        if (!data || data.length < 4) return null;
        return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(0, true);
    }

    /**
     * Read u64 at virtual address
     */
    readU64(va: bigint): bigint | null {
        const data = this.read(va, 8);
        if (!data || data.length < 8) return null;
        return new DataView(data.buffer, data.byteOffset, data.byteLength).getBigUint64(0, true);
    }

    /**
     * Read string at virtual address
     */
    readString(va: bigint, maxLen: number = 16): string | null {
        const data = this.read(va, maxLen);
        if (!data) return null;

        let str = '';
        for (let i = 0; i < data.length && data[i] !== 0; i++) {
            if (data[i] >= 32 && data[i] < 127) {
                str += String.fromCharCode(data[i]);
            }
        }
        return str;
    }

    /**
     * Walk kernel linked list
     */
    walkList(headVA: bigint, offset: number, max: number = 100): bigint[] {
        const entries: bigint[] = [];

        const firstNext = this.readU64(headVA);
        if (!firstNext || firstNext === 0n) return entries;

        let current = firstNext;
        const first = current;

        for (let i = 0; i < max; i++) {
            const structAddr = current - BigInt(offset);
            entries.push(structAddr);

            const next = this.readU64(current);
            if (!next || next === first || next === headVA) break;

            current = next;
        }

        return entries;
    }

    // ===== Process discovery code from kernel-discovery-paged =====

    /**
     * Check if value is a kernel pointer
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
     * Count case transitions in a string (for validation)
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
     * Check if offset contains a valid task_struct
     * EXACT copy from kernel-discovery-paged
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
        const isKernel = mmStripped === 0n;

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
        if (mmStripped === 0n || mmStripped >= 0xffff000000000000n) validityScore += 1;

        // Require higher score for acceptance
        if (validityScore < 3) {
            return null;
        }

        // mm_struct validation - should be 0 (kernel thread) or kernel VA (user process)
        if (mmStripped !== 0n) {
            // For user processes, mm_struct should be a kernel VA (0xffff...)
            // Or it could be a physical address (less common)
            if (mmStripped < 0xffff000000000000n) {
                // Possibly a physical address - let's be more lenient here
                if (mmStripped >= 0x40000000n && mmStripped < 0x200000000n) {
                    // Looks like a plausible physical address
                } else {
                    return null;  // Neither kernel VA nor plausible physical
                }
            }
        }

        // Try to get PGD if mm_struct looks valid
        let pgdPtr = 0;
        if (mmStripped && mmStripped >= BigInt(MemoryConfig.GUEST_RAM_START) && mmStripped < BigInt(MemoryConfig.GUEST_RAM_END)) {
            const mmOffset = Number(mmStripped - BigInt(MemoryConfig.GUEST_RAM_START));
            pgdPtr = Number(this.memory.readU64(mmOffset + KernelConstants.PGD_OFFSET_IN_MM) || 0n);
        }

        // Read files pointer (can be null for kernel threads)
        const filesRaw = this.memory.readU64(offset + 0x990) || 0n;  // task.files offset
        const filesPtr = stripPAC(VA(filesRaw));

        return {
            pid: pid,
            name: name,
            taskStruct: PA(Number(MemoryConfig.GUEST_RAM_START) + offset),
            isKernelThread: isKernel,
            tasksNext: PA(tasksNext),
            tasksPrev: PA(tasksPrev),
            mmStruct: mmStripped,
            pgd: pgdPtr ? PA(pgdPtr) : PA(0),
            files: filesPtr !== VA(0) ? filesPtr : undefined,
            ptes: [],
            sections: []
        };
    }

    // Add this property for checkTaskStruct debug code
    private processes = new Map<number, ProcessInfo>();

    /**
     * Find all processes by scanning for task_structs
     * EXACT copy from kernel-discovery-paged
     */
    findProcesses(): Map<number, ProcessInfo> {
        const knownFound: string[] = [];
        const totalSize = this.memory.getTotalSize();

        // Clear previous results
        this.processes.clear();

        for (let pageStart = 0; pageStart < totalSize; pageStart += KernelConstants.PAGE_SIZE) {
            if (pageStart % (500 * 1024 * 1024) === 0) {  // Report every 500MB
                console.log(`  Scanning ${pageStart / (1024 * 1024)}MB... (${this.processes.size} processes)`);
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

        console.log(`Found ${this.processes.size} processes`);
        return this.processes;
    }
}