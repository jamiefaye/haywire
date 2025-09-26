/**
 * Kernel Memory Helper
 * Simple module for reading kernel memory with VA translation
 */

import { PagedMemory } from './paged-memory';

// Structure offsets from pahole
export const OFFSETS = {
    // task_struct
    'task.files': 0x9B8,
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
     * Translate kernel virtual address to physical
     */
    translateVA(va: bigint): number | null {
        // Linear map (most common case)
        if (va >= 0xffff800000000000n && va < 0xffff800080000000n) {
            return Number(va - 0xffff800000000000n + 0x40000000n);
        }

        // Page table walk for other addresses
        const pgdIndex = Number((va >> 39n) & 0x1FFn);
        const pudIndex = Number((va >> 30n) & 0x1FFn);
        const pmdIndex = Number((va >> 21n) & 0x1FFn);
        const pteIndex = Number((va >> 12n) & 0x1FFn);
        const offset = Number(va & 0xFFFn);

        // Read PGD entry
        const pgdData = this.memory.readBytes((this.kernelPgd - 0x40000000) + pgdIndex * 8, 8);
        if (!pgdData) return null;

        const pgdEntry = new DataView(pgdData.buffer, pgdData.byteOffset, pgdData.byteLength).getBigUint64(0, true);
        if ((pgdEntry & 0x3n) !== 0x3n) return null;

        const pudPA = Number(pgdEntry & 0xFFFFFFFFF000n);

        // Read PUD entry
        const pudData = this.memory.readBytes((pudPA - 0x40000000) + pudIndex * 8, 8);
        if (!pudData) return null;

        const pudEntry = new DataView(pudData.buffer, pudData.byteOffset, pudData.byteLength).getBigUint64(0, true);
        if ((pudEntry & 0x3n) !== 0x3n) return null;

        const pmdPA = Number(pudEntry & 0xFFFFFFFFF000n);

        // Read PMD entry
        const pmdData = this.memory.readBytes((pmdPA - 0x40000000) + pmdIndex * 8, 8);
        if (!pmdData) return null;

        const pmdEntry = new DataView(pmdData.buffer, pmdData.byteOffset, pmdData.byteLength).getBigUint64(0, true);
        if ((pmdEntry & 0x3n) !== 0x3n) return null;

        const ptePA = Number(pmdEntry & 0xFFFFFFFFF000n);

        // Read PTE entry
        const pteData = this.memory.readBytes((ptePA - 0x40000000) + pteIndex * 8, 8);
        if (!pteData) return null;

        const pteEntry = new DataView(pteData.buffer, pteData.byteOffset, pteData.byteLength).getBigUint64(0, true);
        if ((pteEntry & 0x3n) !== 0x3n) return null;

        const pagePA = Number(pteEntry & 0xFFFFFFFFF000n);
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
}