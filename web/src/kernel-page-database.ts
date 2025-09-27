/**
 * Kernel Page Database
 * Stores and indexes PTE and section data for fast page reference lookups
 */

import type { ProcessInfo, PTE, MemorySection, DiscoveryOutput } from './kernel-discovery';

export interface PageReference {
    pid: number;
    processName: string;
    type: 'pte' | 'section';
    virtualAddress: number | bigint;
    permissions: string;
    sectionType?: string;  // For sections: code, data, heap, stack, etc.
    size?: number;         // For sections: size in bytes
}

export interface PageInfo {
    physicalAddress: number;
    references: PageReference[];
    isKernel: boolean;
    isShared: boolean;
    isZero: boolean;
    notFound?: boolean;  // Flag to indicate no information was found
}

export class KernelPageDatabase {
    // Main index: physical page number -> page info
    private pageIndex = new Map<number, PageInfo>();

    // Process info cache
    private processes = new Map<number, ProcessInfo>();

    // Statistics
    private totalPages = 0;
    private totalReferences = 0;
    private sharedPages = 0;
    private kernelPages = 0;
    private userPages = 0;

    constructor() {
        console.log('KernelPageDatabase initialized');
    }

    /**
     * Populate database from kernel discovery output
     */
    public populate(discoveryOutput: DiscoveryOutput): void {
        console.log('Populating kernel page database...');
        console.log('Discovery output:', {
            processes: discoveryOutput.processes?.length || 0,
            hasPtesByPid: !!discoveryOutput.ptesByPid,
            hasSectionsByPid: !!discoveryOutput.sectionsByPid,
            kernelPtes: discoveryOutput.kernelPtes?.length || 0
        });

        // Clear existing data
        this.clear();

        // Store process info
        for (const process of discoveryOutput.processes) {
            this.processes.set(process.pid, process);

            // If we have a task_struct address, add it as a reference
            if (process.taskStruct && Number(process.taskStruct) !== 0) {
                const pageNum = Math.floor(Number(process.taskStruct) / 4096);
                let pageInfo = this.pageIndex.get(pageNum);
                if (!pageInfo) {
                    pageInfo = {
                        physicalAddress: pageNum * 4096,
                        references: [],
                        isKernel: true,
                        isShared: false,
                        isZero: false
                    };
                    this.pageIndex.set(pageNum, pageInfo);
                }

                pageInfo.references.push({
                    pid: process.pid,
                    processName: process.name,
                    type: 'section',
                    virtualAddress: process.taskStruct,
                    permissions: 'task_struct',
                    sectionType: 'kernel'
                });
                this.totalReferences++;
            }
        }

        // Index all PTEs by physical page
        this.indexPTEs(discoveryOutput);

        // Index all memory sections
        this.indexSections(discoveryOutput);

        // Add kernel PTEs
        this.indexKernelPTEs(discoveryOutput.kernelPtes);

        // Calculate statistics
        this.calculateStatistics();

        console.log(`Database populated: ${this.pageIndex.size} pages, ${this.totalReferences} references`);
    }

    /**
     * Index PTEs from all processes
     */
    private indexPTEs(discoveryOutput: DiscoveryOutput): void {
        if (!discoveryOutput.ptesByPid) {
            console.log('No PTEs found in discovery output');
            return;
        }

        for (const [pid, ptes] of discoveryOutput.ptesByPid) {
            const process = this.processes.get(pid);
            if (!process || !ptes || ptes.length === 0) continue;

            for (const pte of ptes) {
                if (!pte.pa || Number(pte.pa) === 0) continue;
                const pageNum = Math.floor(Number(pte.pa) / 4096);

                // Get or create page info
                let pageInfo = this.pageIndex.get(pageNum);
                if (!pageInfo) {
                    pageInfo = {
                        physicalAddress: pageNum * 4096,
                        references: [],
                        isKernel: false,
                        isShared: false,
                        isZero: false
                    };
                    this.pageIndex.set(pageNum, pageInfo);
                }

                // Add PTE reference
                pageInfo.references.push({
                    pid: pid,
                    processName: process.name,
                    type: 'pte',
                    virtualAddress: pte.va,
                    permissions: this.formatPermissions(pte),
                });

                this.totalReferences++;
            }
        }
    }

    /**
     * Index memory sections from all processes
     */
    private indexSections(discoveryOutput: DiscoveryOutput): void {
        if (!discoveryOutput.sectionsByPid) {
            console.log('No sections found in discovery output');
            return;
        }

        for (const [pid, sections] of discoveryOutput.sectionsByPid) {
            const process = this.processes.get(pid);
            if (!process || !sections || sections.length === 0) continue;

            for (const section of sections) {
                if (!section.startPa || section.size === 0) continue;
                // Sections describe virtual address ranges
                // We need to find their physical pages through PTEs
                const startPage = Math.floor(Number(section.startPa) / 4096);
                const endPage = Math.ceil((Number(section.startPa) + section.size) / 4096);

                for (let pageNum = startPage; pageNum < endPage; pageNum++) {
                    let pageInfo = this.pageIndex.get(pageNum);
                    if (!pageInfo) {
                        pageInfo = {
                            physicalAddress: pageNum * 4096,
                            references: [],
                            isKernel: section.type === 'kernel',
                            isShared: false,
                            isZero: false
                        };
                        this.pageIndex.set(pageNum, pageInfo);
                    }

                    // Add section reference if not already present
                    const existingRef = pageInfo.references.find(
                        ref => ref.pid === pid &&
                               ref.type === 'section' &&
                               ref.sectionType === section.type
                    );

                    if (!existingRef) {
                        pageInfo.references.push({
                            pid: pid,
                            processName: process.name,
                            type: 'section',
                            virtualAddress: section.startVa,
                            permissions: this.formatSectionFlags(Number(section.flags)),
                            sectionType: section.type,
                            size: section.size
                        });
                        this.totalReferences++;
                    }
                }
            }
        }
    }

    /**
     * Index kernel PTEs
     */
    private indexKernelPTEs(kernelPtes: PTE[]): void {
        if (!kernelPtes || kernelPtes.length === 0) {
            console.log('No kernel PTEs found in discovery output');
            return;
        }

        for (const pte of kernelPtes) {
            if (!pte.pa || Number(pte.pa) === 0) continue;
            const pageNum = Math.floor(Number(pte.pa) / 4096);

            let pageInfo = this.pageIndex.get(pageNum);
            if (!pageInfo) {
                pageInfo = {
                    physicalAddress: pageNum * 4096,
                    references: [],
                    isKernel: true,
                    isShared: false,
                    isZero: false
                };
                this.pageIndex.set(pageNum, pageInfo);
            }

            pageInfo.isKernel = true;
            pageInfo.references.push({
                pid: 0,
                processName: 'kernel',
                type: 'pte',
                virtualAddress: pte.va,
                permissions: this.formatPermissions(pte),
            });

            this.totalReferences++;
        }
    }

    /**
     * Calculate database statistics
     */
    private calculateStatistics(): void {
        this.totalPages = this.pageIndex.size;
        this.sharedPages = 0;
        this.kernelPages = 0;
        this.userPages = 0;

        for (const pageInfo of this.pageIndex.values()) {
            // Check if shared (referenced by multiple processes)
            const uniquePids = new Set(pageInfo.references.map(ref => ref.pid));
            if (uniquePids.size > 1) {
                pageInfo.isShared = true;
                this.sharedPages++;
            }

            if (pageInfo.isKernel) {
                this.kernelPages++;
            } else {
                this.userPages++;
            }
        }
    }

    /**
     * Get page references for a physical address
     */
    public getPageReferences(physicalAddress: number): PageReference[] {
        const pageNum = Math.floor(physicalAddress / 4096);
        const pageInfo = this.pageIndex.get(pageNum);
        return pageInfo?.references || [];
    }

    /**
     * Get detailed page info for a physical address
     */
    public getPageInfo(physicalAddress: number): PageInfo | null {
        const pageNum = Math.floor(physicalAddress / 4096);
        return this.pageIndex.get(pageNum) || null;
    }

    /**
     * Get pages referenced by a specific process
     */
    public getProcessPages(pid: number): number[] {
        const pages: number[] = [];
        for (const [pageNum, pageInfo] of this.pageIndex) {
            if (pageInfo.references.some(ref => ref.pid === pid)) {
                pages.push(pageNum * 4096);
            }
        }
        return pages;
    }

    /**
     * Get shared pages (referenced by multiple processes)
     */
    public getSharedPages(): Map<number, number[]> {
        const sharedPages = new Map<number, number[]>();

        for (const [pageNum, pageInfo] of this.pageIndex) {
            const pids = [...new Set(pageInfo.references.map(ref => ref.pid))];
            if (pids.length > 1) {
                sharedPages.set(pageNum * 4096, pids);
            }
        }

        return sharedPages;
    }

    /**
     * Format PTE permissions for display
     */
    private formatPermissions(pte: PTE): string {
        return `${pte.r ? 'R' : '-'}${pte.w ? 'W' : '-'}${pte.x ? 'X' : '-'}`;
    }

    /**
     * Format section flags for display
     */
    private formatSectionFlags(flags: number): string {
        const perms: string[] = [];
        if (flags & 0x1) perms.push('R');
        if (flags & 0x2) perms.push('W');
        if (flags & 0x4) perms.push('X');
        if (flags & 0x8) perms.push('S'); // Shared
        return perms.join('') || 'none';
    }

    /**
     * Get statistics
     */
    public getStatistics() {
        return {
            totalPages: this.totalPages,
            totalReferences: this.totalReferences,
            sharedPages: this.sharedPages,
            kernelPages: this.kernelPages,
            userPages: this.userPages,
            uniqueProcesses: this.processes.size
        };
    }

    /**
     * Clear the database
     */
    public clear(): void {
        this.pageIndex.clear();
        this.processes.clear();
        this.totalPages = 0;
        this.totalReferences = 0;
        this.sharedPages = 0;
        this.kernelPages = 0;
        this.userPages = 0;
    }

    /**
     * Check if database has data
     */
    public hasData(): boolean {
        return this.pageIndex.size > 0;
    }
}

// Singleton instance
export const kernelPageDB = new KernelPageDatabase();