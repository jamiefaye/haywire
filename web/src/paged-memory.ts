/**
 * Paged Memory Management for Large Files
 * Handles memory as an array of page objects to avoid JavaScript array size limits
 */

export const PAGE_SIZE = 4096; // 4KB pages

export interface MemoryPage {
    pageNumber: number;      // Page index (0-based)
    physicalAddress: number;  // Physical address in guest memory
    data: Uint8Array;        // 4KB of data
}

export class PagedMemory {
    private pages: Map<number, MemoryPage>;
    private totalSize: number;
    private pageCount: number;

    constructor(totalSize: number) {
        this.totalSize = totalSize;
        this.pageCount = Math.ceil(totalSize / PAGE_SIZE);
        this.pages = new Map();
    }

    /**
     * Add a page to memory
     */
    addPage(pageNumber: number, data: Uint8Array): void {
        if (data.length !== PAGE_SIZE) {
            throw new Error(`Page data must be exactly ${PAGE_SIZE} bytes`);
        }

        const physicalAddress = pageNumber * PAGE_SIZE + 0x40000000; // Guest RAM start
        this.pages.set(pageNumber, {
            pageNumber,
            physicalAddress,
            data: data
        });
    }

    /**
     * Get total size of memory
     */
    getTotalSize(): number {
        return this.totalSize;
    }

    /**
     * Read bytes from paged memory
     */
    read(offset: number, length: number): Uint8Array | null {
        if (offset < 0 || offset + length > this.totalSize) {
            return null;
        }

        const result = new Uint8Array(length);
        let resultOffset = 0;

        while (resultOffset < length) {
            const currentOffset = offset + resultOffset;
            const pageNumber = Math.floor(currentOffset / PAGE_SIZE);
            const pageOffset = currentOffset % PAGE_SIZE;
            const bytesToRead = Math.min(PAGE_SIZE - pageOffset, length - resultOffset);

            const page = this.pages.get(pageNumber);
            if (!page) {
                // Page not loaded, return zeros
                for (let i = 0; i < bytesToRead; i++) {
                    result[resultOffset + i] = 0;
                }
            } else {
                // Copy from page data
                result.set(page.data.subarray(pageOffset, pageOffset + bytesToRead), resultOffset);
            }

            resultOffset += bytesToRead;
        }

        return result;
    }

    /**
     * Read raw bytes
     */
    readBytes(offset: number, length: number): Uint8Array | null {
        return this.read(offset, length);
    }

    /**
     * Read a 32-bit value
     */
    readU32(offset: number): number | null {
        const data = this.read(offset, 4);
        if (!data) return null;
        return new DataView(data.buffer).getUint32(0, true);
    }

    /**
     * Read a 64-bit value
     */
    readU64(offset: number): bigint | null {
        const data = this.read(offset, 8);
        if (!data) return null;
        return new DataView(data.buffer).getBigUint64(0, true);
    }

    /**
     * Read a string
     */
    readString(offset: number, maxLen: number = 16): string | null {
        const data = this.read(offset, maxLen);
        if (!data) return null;

        // Find null terminator or use full length
        let endIdx = data.indexOf(0);
        if (endIdx === -1) {
            // No null terminator found, use the whole buffer but check if it's printable
            endIdx = maxLen;
            // Check if at least first few chars are printable
            let printable = 0;
            for (let i = 0; i < Math.min(4, endIdx); i++) {
                if (data[i] >= 32 && data[i] <= 126) printable++;
            }
            if (printable < 2) return null;  // Not enough printable chars
        }

        if (endIdx === 0) return null;  // Empty string

        const decoder = new TextDecoder('ascii', { fatal: false });
        const str = decoder.decode(data.subarray(0, endIdx));

        // Filter out non-printable characters
        return str.replace(/[\x00-\x1F\x7F-\xFF]/g, '');
    }

    /**
     * Get total loaded pages
     */
    getLoadedPageCount(): number {
        return this.pages.size;
    }

    /**
     * Get memory usage estimate
     */
    getMemoryUsage(): string {
        const usedBytes = this.pages.size * PAGE_SIZE;
        const usedMB = (usedBytes / (1024 * 1024)).toFixed(2);
        const totalMB = (this.totalSize / (1024 * 1024)).toFixed(2);
        return `${usedMB}MB / ${totalMB}MB (${this.pages.size} / ${this.pageCount} pages)`;
    }

    /**
     * Load from chunks progressively
     */
    static async fromChunks(chunks: Uint8Array[], onProgress?: (percent: number) => void): Promise<PagedMemory> {
        const totalSize = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
        const memory = new PagedMemory(totalSize);

        let globalOffset = 0;
        let pagesLoaded = 0;
        const totalPages = Math.ceil(totalSize / PAGE_SIZE);

        for (const chunk of chunks) {
            let chunkOffset = 0;

            while (chunkOffset < chunk.length) {
                const remainingInPage = PAGE_SIZE - (globalOffset % PAGE_SIZE);
                const bytesToCopy = Math.min(remainingInPage, chunk.length - chunkOffset);

                const pageNumber = Math.floor(globalOffset / PAGE_SIZE);

                // Get or create page
                let page = memory.pages.get(pageNumber);
                if (!page) {
                    page = {
                        pageNumber,
                        physicalAddress: pageNumber * PAGE_SIZE + 0x40000000,
                        data: new Uint8Array(PAGE_SIZE)
                    };
                    memory.pages.set(pageNumber, page);
                    pagesLoaded++;
                }

                // Copy data into page
                const pageOffset = globalOffset % PAGE_SIZE;
                page.data.set(chunk.subarray(chunkOffset, chunkOffset + bytesToCopy), pageOffset);

                chunkOffset += bytesToCopy;
                globalOffset += bytesToCopy;

                // Report progress
                if (onProgress && pagesLoaded % 100 === 0) {
                    onProgress((pagesLoaded / totalPages) * 100);
                }
            }
        }

        return memory;
    }
}