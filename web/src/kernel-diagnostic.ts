/**
 * Kernel Diagnostic Tool
 * Step-by-step debugging of kernel structure discovery
 */

import { PagedMemory } from './paged-memory';
import { KernelConstants } from './kernel-discovery';

export class KernelDiagnostic {
    private memory: PagedMemory;

    constructor(memory: PagedMemory) {
        this.memory = memory;
    }

    /**
     * Test 1: Find a specific process and trace its mm_struct → PGD
     */
    public async diagnoseProcess(pid: number): Promise<void> {
        console.log(`\n=== DIAGNOSING PID ${pid} ===`);

        // Step 1: Find task_struct
        console.log('\n1. FINDING TASK_STRUCT:');
        const taskStruct = await this.findTaskStructByPid(pid);
        if (!taskStruct) {
            console.log(`   ❌ Could not find task_struct for PID ${pid}`);
            return;
        }

        console.log(`   ✓ Found task_struct at 0x${taskStruct.toString(16)}`);

        // Step 2: Read mm_struct pointer
        console.log('\n2. READING MM_STRUCT:');
        const mmOffset = taskStruct - 0x40000000 + KernelConstants.MM_OFFSET;
        const mmPtr = this.memory.readU64(mmOffset);

        if (!mmPtr || mmPtr === 0n) {
            console.log(`   ❌ mm_struct is NULL (kernel thread?)`);
            return;
        }

        console.log(`   ✓ mm_struct at 0x${mmPtr.toString(16)}`);

        // Step 3: Read PGD from mm_struct
        console.log('\n3. READING PGD FROM MM_STRUCT:');
        const pgdOffset = Number(mmPtr) - 0x40000000 + KernelConstants.PGD_OFFSET_IN_MM;
        const pgdRaw = this.memory.readU64(pgdOffset);

        if (!pgdRaw) {
            console.log(`   ❌ Could not read PGD from mm_struct`);
            return;
        }

        console.log(`   Raw PGD value: 0x${pgdRaw.toString(16)}`);

        // Apply PA_MASK to get physical address
        const pgdPA = pgdRaw & 0x0000FFFFFFFFF000n;
        console.log(`   Physical PGD: 0x${pgdPA.toString(16)}`);

        // Step 4: Try to read first few PGD entries
        console.log('\n4. READING PGD ENTRIES:');
        await this.dumpPageTableEntries(Number(pgdPA), 'PGD', 10);

        // Step 5: Try to read maple tree from mm_struct
        console.log('\n5. CHECKING MAPLE TREE (mm_mt):');
        const mapleTreeOffset = Number(mmPtr) - 0x40000000 + 0x40; // mm_mt at offset 0x40
        const mapleTreeRoot = this.memory.readU64(mapleTreeOffset + 0x48); // ma_root at offset 0x48

        if (mapleTreeRoot) {
            console.log(`   Maple tree root: 0x${mapleTreeRoot.toString(16)}`);
            await this.diagnoseMapleNode(mapleTreeRoot);
        } else {
            console.log(`   ❌ No maple tree root found`);
        }
    }

    /**
     * Test 2: Walk a specific PGD
     */
    public async walkPGD(pgdAddr: number): Promise<void> {
        console.log(`\n=== WALKING PGD AT 0x${pgdAddr.toString(16)} ===`);

        const pgdOffset = pgdAddr - 0x40000000;

        // Check first 10 PGD entries
        for (let i = 0; i < 10; i++) {
            const entry = this.memory.readU64(pgdOffset + i * 8);
            if (!entry || entry === 0n) continue;

            const entryType = Number(entry) & 0x3;
            const physAddr = entry & 0x0000FFFFFFFFF000n;

            console.log(`\nPGD[${i}]: 0x${entry.toString(16)}`);
            console.log(`  Type: ${entryType === 0 ? 'Invalid' : entryType === 1 ? 'Block' : 'Table'}`);
            console.log(`  Physical: 0x${physAddr.toString(16)}`);

            if (entryType === 0x3) {
                // Follow to PUD
                console.log(`  Following to PUD...`);
                await this.dumpPageTableEntries(Number(physAddr), 'PUD', 5);
            }
        }
    }

    /**
     * Test 3: Diagnose maple tree node
     */
    private async diagnoseMapleNode(nodeAddr: bigint, depth: number = 0): Promise<void> {
        if (depth > 3) return; // Limit recursion

        const indent = '  '.repeat(depth);
        console.log(`${indent}Maple node at 0x${nodeAddr.toString(16)}:`);

        // Check if it's a leaf or internal node
        const nodeType = (nodeAddr >> 3n) & 0x0Fn;
        console.log(`${indent}  Type: ${nodeType} (${nodeType <= 1 ? 'leaf' : 'internal'})`);

        if (nodeType <= 1) {
            // Leaf node - contains VMAs
            console.log(`${indent}  This should contain vm_area_structs`);
            // Try to read first VMA
            const vmaOffset = Number(nodeAddr & ~0x3Fn) - 0x40000000;
            if (vmaOffset >= 0) {
                const vmStart = this.memory.readU64(vmaOffset);
                const vmEnd = this.memory.readU64(vmaOffset + 8);
                if (vmStart && vmEnd) {
                    console.log(`${indent}  VMA: 0x${vmStart.toString(16)} - 0x${vmEnd.toString(16)}`);
                }
            }
        } else {
            // Internal node - has children
            console.log(`${indent}  Internal node with children`);
        }
    }

    /**
     * Helper: Find task_struct by PID
     */
    private async findTaskStructByPid(targetPid: number): Promise<number | null> {
        const fileSize = this.memory.getTotalSize();
        const pageSize = 4096;

        // Scan memory for task_structs
        for (let offset = 0; offset < fileSize; offset += pageSize) {
            // Check SLAB offsets
            for (const slabOffset of [0x0, 0x2380, 0x4700]) {
                const taskOffset = offset + slabOffset;
                if (taskOffset + KernelConstants.TASK_STRUCT_SIZE > fileSize) continue;

                // Read PID
                const pidValue = this.memory.readU32(taskOffset + KernelConstants.PID_OFFSET);
                if (pidValue === targetPid) {
                    return taskOffset + 0x40000000; // Return physical address
                }
            }
        }

        return null;
    }

    /**
     * Helper: Dump page table entries
     */
    private async dumpPageTableEntries(tableAddr: number, level: string, count: number): Promise<void> {
        const tableOffset = tableAddr - 0x40000000;

        console.log(`   ${level} entries at 0x${tableAddr.toString(16)}:`);

        let validCount = 0;
        for (let i = 0; i < count; i++) {
            const entry = this.memory.readU64(tableOffset + i * 8);
            if (!entry || entry === 0n) continue;

            validCount++;
            const entryType = Number(entry) & 0x3;
            const physAddr = entry & 0x0000FFFFFFFFF000n;

            console.log(`     [${i}]: 0x${entry.toString(16)} -> PA: 0x${physAddr.toString(16)} (type: ${entryType})`);
        }

        if (validCount === 0) {
            console.log(`     ❌ No valid entries found`);
        }
    }

    /**
     * Test 4: Find and validate swapper_pg_dir
     */
    public async findSwapperPgDir(): Promise<void> {
        console.log('\n=== SEARCHING FOR SWAPPER_PG_DIR ===');

        // Known location from QMP
        const knownSwapper = 0x136dbf000;
        console.log(`\n1. Checking known location 0x${knownSwapper.toString(16)}:`);
        await this.dumpPageTableEntries(knownSwapper, 'SWAPPER_PGD', 10);

        // Try to find it by pattern
        console.log('\n2. Searching for PGD patterns:');
        const candidates = await this.findPGDCandidates();
        console.log(`   Found ${candidates.length} PGD candidates`);

        for (const candidate of candidates.slice(0, 5)) {
            console.log(`\n   Candidate at 0x${candidate.toString(16)}:`);
            await this.dumpPageTableEntries(candidate, 'PGD', 5);
        }
    }

    /**
     * Helper: Find PGD candidates by pattern
     */
    private async findPGDCandidates(): Promise<number[]> {
        const candidates: number[] = [];
        const fileSize = this.memory.getTotalSize();
        const pageSize = 4096;

        for (let offset = 0; offset < fileSize - pageSize; offset += pageSize) {
            // Quick check: does this page look like a page table?
            let validEntries = 0;

            for (let i = 0; i < 5; i++) {
                const entry = this.memory.readU64(offset + i * 8);
                if (!entry) continue;

                const entryType = Number(entry) & 0x3;
                if (entryType === 0x3 || entryType === 0x1) {
                    validEntries++;
                }
            }

            if (validEntries >= 2) {
                candidates.push(offset + 0x40000000);
            }

            if (candidates.length >= 100) break; // Limit results
        }

        return candidates;
    }

    /**
     * Run full diagnostic suite
     */
    public async runFullDiagnostic(): Promise<void> {
        console.log('==================================================');
        console.log('        KERNEL STRUCTURE DIAGNOSTIC TOOL         ');
        console.log('==================================================');

        // Test known PIDs
        const testPids = [1, 2, 645]; // init, kthreadd, sshd

        for (const pid of testPids) {
            await this.diagnoseProcess(pid);
        }

        // Test swapper_pg_dir
        await this.findSwapperPgDir();

        // Test specific PGD if known
        const knownPGD = 0x136dbf000;
        await this.walkPGD(knownPGD);

        console.log('\n==================================================');
        console.log('           DIAGNOSTIC COMPLETE                    ');
        console.log('==================================================');
    }
}

// Export a function to run diagnostics
export async function runKernelDiagnostics(memory: PagedMemory): Promise<void> {
    const diagnostic = new KernelDiagnostic(memory);
    await diagnostic.runFullDiagnostic();
}