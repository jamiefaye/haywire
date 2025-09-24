/**
 * PageInfo - Unified page tracking system for Haywire
 *
 * Tracks everything about each page in the system:
 * - Who maps it (which PIDs)
 * - How it's mapped (permissions)
 * - What it contains (type)
 * - Virtual and physical addresses
 */

export interface PageInfo {
    // Core addressing
    pa: number;           // Physical address (unique key)
    size: number;         // Page size (4KB, 2MB, 1GB)

    // Virtual mappings - who maps this page and where
    mappings: PageMapping[];

    // Content type detection
    contentType: PageContentType;
    contentHint?: string;  // e.g., "libc.so.6 .text", "stack guard page"

    // Statistics
    lastAccessed?: number;  // For hot/cold analysis
    accessCount?: number;   // How often accessed
    isDirty?: boolean;      // Modified since mapped

    // Kernel info
    isKernel: boolean;      // Kernel page?
    kernelStruct?: string;  // e.g., "task_struct", "mm_struct", "page table"
}

export interface PageMapping {
    pid: number;
    processName: string;
    va: number;           // Virtual address in this process
    perms: string;        // 'rwxp' style permissions
    sectionType: string;  // 'code', 'data', 'heap', 'stack', etc.
    vmaStart?: number;    // Start of containing VMA
    vmaEnd?: number;      // End of containing VMA
}

export type PageContentType =
    | 'code'           // Executable code
    | 'rodata'         // Read-only data
    | 'data'           // Initialized data
    | 'bss'            // Uninitialized data
    | 'heap'           // Heap allocation
    | 'stack'          // Stack
    | 'guard'          // Guard page
    | 'shared_lib'     // Shared library
    | 'page_table'     // Page table (PGD/PUD/PMD/PTE)
    | 'kernel_struct'  // Kernel data structure
    | 'zero'           // Zero page
    | 'unknown';

/**
 * PageCollection - Manages collections of PageInfo
 */
export class PageCollection {
    private pages = new Map<number, PageInfo>();  // PA → PageInfo
    private vaToPA = new Map<string, number>();   // "pid:va" → PA
    private pidPages = new Map<number, Set<number>>(); // PID → Set of PAs

    /**
     * Add a page mapping from PTE walk
     */
    addPTEMapping(pid: number, processName: string, va: number, pa: number, flags: number, size: number = 4096): void {
        // Get or create PageInfo
        let page = this.pages.get(pa);
        if (!page) {
            page = {
                pa,
                size,
                mappings: [],
                contentType: 'unknown',
                isKernel: false
            };
            this.pages.set(pa, page);
        }

        // Add mapping
        const perms = this.decodePermissions(flags);
        page.mappings.push({
            pid,
            processName,
            va,
            perms,
            sectionType: this.guessSectionType(va, flags)
        });

        // Update indices
        this.vaToPA.set(`${pid}:${va}`, pa);
        if (!this.pidPages.has(pid)) {
            this.pidPages.set(pid, new Set());
        }
        this.pidPages.get(pid)!.add(pa);

        // Update content type based on permissions
        if (!page.contentType || page.contentType === 'unknown') {
            page.contentType = this.guessContentType(va, flags);
        }
    }

    /**
     * Enhance pages with VMA information
     */
    addVMAInfo(pid: number, vmaStart: number, vmaEnd: number, sectionType: string, perms: string): void {
        // Find all pages in this VMA range
        for (let va = vmaStart; va < vmaEnd; va += 4096) {
            const key = `${pid}:${va}`;
            const pa = this.vaToPA.get(key);

            if (pa) {
                const page = this.pages.get(pa);
                if (page) {
                    // Find and update the mapping
                    const mapping = page.mappings.find(m => m.pid === pid && m.va === va);
                    if (mapping) {
                        mapping.vmaStart = vmaStart;
                        mapping.vmaEnd = vmaEnd;
                        mapping.sectionType = sectionType;
                        mapping.perms = perms;
                    }
                }
            }
        }
    }

    /**
     * Mark kernel pages
     */
    markKernelPage(pa: number, structType?: string): void {
        const page = this.pages.get(pa);
        if (page) {
            page.isKernel = true;
            if (structType) {
                page.kernelStruct = structType;
                page.contentType = 'kernel_struct';
            }
        }
    }

    /**
     * Get page info for a physical address
     */
    getPageByPA(pa: number): PageInfo | undefined {
        return this.pages.get(pa & ~0xFFF); // Mask to page boundary
    }

    /**
     * Get page info for a virtual address
     */
    getPageByVA(pid: number, va: number): PageInfo | undefined {
        const key = `${pid}:${va & ~0xFFF}`;
        const pa = this.vaToPA.get(key);
        return pa ? this.pages.get(pa) : undefined;
    }

    /**
     * Translate VA to PA for a process
     */
    translateVA(pid: number, va: number): number | undefined {
        const key = `${pid}:${va & ~0xFFF}`;
        const pa = this.vaToPA.get(key);
        return pa ? pa | (va & 0xFFF) : undefined;
    }

    /**
     * Get all pages mapped by a process
     */
    getProcessPages(pid: number): PageInfo[] {
        const pas = this.pidPages.get(pid);
        if (!pas) return [];

        return Array.from(pas).map(pa => this.pages.get(pa)!).filter(p => p);
    }

    /**
     * Add section info from VMA walk
     */
    addSectionInfo(pid: number, processName: string, section: any): void {
        // Sections provide VA ranges but not PA mappings
        // We'll correlate these with PTEs later
        // For now, just track that this process has this VA range

        // This helps us understand the memory layout even if we don't have
        // all the PTEs for the region

        // We could store sections separately or enhance PageMapping
        // For now, this is a placeholder for future enhancement
    }

    /**
     * Get shared pages (mapped by multiple processes)
     */
    getSharedPages(): PageInfo[] {
        return Array.from(this.pages.values()).filter(p => p.mappings.length > 1);
    }

    /**
     * Build tooltip text for a page
     */
    getPageTooltip(pa: number): string {
        const page = this.pages.get(pa);
        if (!page) return `Unknown page at PA 0x${pa.toString(16)}`;

        const lines: string[] = [];
        lines.push(`Physical: 0x${pa.toString(16)} (${page.size / 1024}KB page)`);
        lines.push(`Type: ${page.contentType}${page.contentHint ? ` (${page.contentHint})` : ''}`);

        if (page.isKernel) {
            lines.push(`Kernel: ${page.kernelStruct || 'kernel page'}`);
        }

        lines.push(`Mapped by ${page.mappings.length} process(es):`);
        page.mappings.slice(0, 5).forEach(m => {
            lines.push(`  • PID ${m.pid} (${m.processName})`);
            lines.push(`    VA: 0x${m.va.toString(16)} [${m.perms}] ${m.sectionType}`);
            if (m.vmaStart !== undefined) {
                lines.push(`    VMA: 0x${m.vmaStart.toString(16)}-0x${m.vmaEnd!.toString(16)}`);
            }
        });

        if (page.mappings.length > 5) {
            lines.push(`  ... and ${page.mappings.length - 5} more`);
        }

        return lines.join('\n');
    }

    /**
     * Create a crunched address space (only mapped pages)
     */
    getCrunchedSpace(filter?: {
        pid?: number;
        contentType?: PageContentType;
        sharedOnly?: boolean;
        kernelOnly?: boolean;
    }): CrunchedAddressSpace {
        let pages = Array.from(this.pages.values());

        // Apply filters
        if (filter) {
            if (filter.pid !== undefined) {
                pages = pages.filter(p => p.mappings.some(m => m.pid === filter.pid));
            }
            if (filter.contentType) {
                pages = pages.filter(p => p.contentType === filter.contentType);
            }
            if (filter.sharedOnly) {
                pages = pages.filter(p => p.mappings.length > 1);
            }
            if (filter.kernelOnly) {
                pages = pages.filter(p => p.isKernel);
            }
        }

        // Sort by PA
        pages.sort((a, b) => a.pa - b.pa);

        return new CrunchedAddressSpace(pages);
    }

    /**
     * Get statistics
     */
    getStats(): PageCollectionStats {
        const stats = {
            totalPages: this.pages.size,
            totalMappings: 0,
            uniqueProcesses: new Set<number>(),
            sharedPages: 0,
            kernelPages: 0,
            byType: new Map<PageContentType, number>(),
            avgMappingsPerPage: 0
        };

        for (const page of this.pages.values()) {
            stats.totalMappings += page.mappings.length;
            page.mappings.forEach(m => stats.uniqueProcesses.add(m.pid));

            if (page.mappings.length > 1) stats.sharedPages++;
            if (page.isKernel) stats.kernelPages++;

            const count = stats.byType.get(page.contentType) || 0;
            stats.byType.set(page.contentType, count + 1);
        }

        stats.avgMappingsPerPage = stats.totalMappings / stats.totalPages;

        return {
            ...stats,
            uniqueProcesses: stats.uniqueProcesses.size
        };
    }

    private decodePermissions(flags: number): string {
        // ARM64 PTE flags
        let perms = '';
        perms += (flags & 0x1) ? 'r' : '-';  // Present implies readable
        perms += (flags & 0x80) ? '-' : 'w'; // Bit 7: AP[2] (0=RW, 1=RO)
        perms += (flags & 0x10) ? '-' : 'x'; // Bit 4: UXN (0=exec, 1=no-exec)
        perms += 'p'; // Always private for now (could check shared)
        return perms;
    }

    private guessSectionType(va: number, flags: number): string {
        if (va >= 0x7fff00000000) return 'stack';
        if (va >= 0x7f0000000000) return 'lib';
        if (va >= 0x400000 && va < 0x500000) return 'code';
        if (va >= 0x600000 && va < 0x700000) return 'data';
        return 'anon';
    }

    private guessContentType(va: number, flags: number): PageContentType {
        const exec = !(flags & 0x10);
        const write = !(flags & 0x80);

        if (exec && !write) return 'code';
        if (!exec && !write) return 'rodata';
        if (!exec && write) {
            if (va >= 0x7fff00000000) return 'stack';
            if (va >= 0x600000 && va < 0x700000) return 'data';
            return 'heap';
        }
        return 'unknown';
    }
}

/**
 * Crunched address space - sequential view of mapped pages only
 */
export class CrunchedAddressSpace {
    constructor(public pages: PageInfo[]) {}

    /**
     * Get page at crunched offset
     */
    getPageAtOffset(offset: number): PageInfo | undefined {
        const pageIndex = Math.floor(offset / 4096);
        return this.pages[pageIndex];
    }

    /**
     * Convert crunched offset to physical address
     */
    offsetToPA(offset: number): number {
        const pageIndex = Math.floor(offset / 4096);
        const pageOffset = offset & 0xFFF;

        if (pageIndex >= this.pages.length) return 0;
        return this.pages[pageIndex].pa + pageOffset;
    }

    /**
     * Convert physical address to crunched offset
     */
    paToOffset(pa: number): number | undefined {
        const pagePA = pa & ~0xFFF;
        const pageOffset = pa & 0xFFF;

        const index = this.pages.findIndex(p => p.pa === pagePA);
        if (index === -1) return undefined;

        return (index * 4096) + pageOffset;
    }

    /**
     * Get total size of crunched space
     */
    getTotalSize(): number {
        return this.pages.length * 4096;
    }
}

interface PageCollectionStats {
    totalPages: number;
    totalMappings: number;
    uniqueProcesses: number;
    sharedPages: number;
    kernelPages: number;
    byType: Map<PageContentType, number>;
    avgMappingsPerPage: number;
}