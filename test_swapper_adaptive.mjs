#!/usr/bin/env node

import fs from 'fs';

// Constants
const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const PA_MASK = 0x0000FFFFFFFFF000n;

// Helper to read a page from file
function readPage(fd, fileOffset) {
    const buffer = Buffer.allocUnsafe(PAGE_SIZE);
    fs.readSync(fd, buffer, 0, PAGE_SIZE, fileOffset);
    return buffer;
}

// Analyze a potential swapper_pg_dir
function analyzeSwapperCandidate(fd, fileSize, physAddr) {
    const offset = physAddr - GUEST_RAM_START;
    if (offset < 0 || offset + PAGE_SIZE > fileSize) {
        return null;
    }

    const page = readPage(fd, offset);
    const analysis = {
        physAddr,
        score: 0,
        reasons: [],
        pgd0PudCount: 0,
        memSizeEstimate: 0
    };

    // Count valid entries
    let userEntries = 0;
    let kernelEntries = [];
    let pgd0Entry = null;

    for (let i = 0; i < 512; i++) {
        const entry = page.readBigUint64LE(i * 8);
        const entryType = Number(entry & 0x3n);

        if (entryType === 0x1 || entryType === 0x3) {
            if (i === 0) {
                pgd0Entry = {
                    type: entryType === 0x1 ? 'BLOCK' : 'TABLE',
                    pa: Number(entry & PA_MASK)
                };
            }
            if (i < 256) {
                userEntries++;
            } else {
                kernelEntries.push(i);
            }
        }
    }

    // Must have PGD[0] and sparse structure
    if (!pgd0Entry) return null;
    const totalEntries = userEntries + kernelEntries.length;
    if (totalEntries < 2 || totalEntries > 20) return null;

    // Check PUD count if PGD[0] is a TABLE
    if (pgd0Entry.type === 'TABLE') {
        const pudOffset = pgd0Entry.pa - GUEST_RAM_START;
        if (pudOffset >= 0 && pudOffset + PAGE_SIZE <= fileSize) {
            const pudPage = readPage(fd, pudOffset);
            let pudCount = 0;
            let consecutivePuds = true;

            for (let j = 0; j < 512; j++) {
                const pud = pudPage.readBigUint64LE(j * 8);
                const pudType = Number(pud & 0x3n);
                if (pudType >= 1) {
                    pudCount++;
                    // Check if PUDs are NOT consecutive from index 0
                    if (pudCount > 1 && j !== pudCount - 1) {
                        consecutivePuds = false;
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
    const hasKernelStart = kernelEntries.includes(256);
    const hasHighKernel = kernelEntries.some(idx => idx >= 500);

    if (hasKernelStart) {
        analysis.score += 1;
        analysis.reasons.push('Has kernel text mapping (PGD[256])');
    }

    if (hasHighKernel) {
        analysis.score += 1;
        analysis.reasons.push(`Has high kernel mappings (${kernelEntries.filter(i => i >= 500).join(', ')})`);
    }

    // Prefer single user entry (just PGD[0])
    if (userEntries === 1) {
        analysis.score += 1;
        analysis.reasons.push('Single user entry (expected)');
    }

    // Multiple kernel entries indicate complete kernel mapping
    if (kernelEntries.length >= 2) {
        analysis.score += 1;
        analysis.reasons.push(`${kernelEntries.length} kernel entries`);
    }

    // Check for I/O mapping patterns (entries in 256-511 range)
    const midKernelEntries = kernelEntries.filter(i => i > 256 && i < 500);
    if (midKernelEntries.length > 0) {
        analysis.reasons.push(`Possible I/O mappings at PGD[${midKernelEntries.join(', ')}]`);
    }

    analysis.summary = `PA: 0x${physAddr.toString(16)}, Score: ${analysis.score}, ` +
                       `Est. RAM: ${analysis.memSizeEstimate}GB, ` +
                       `User: ${userEntries}, Kernel: ${kernelEntries.length}`;

    return analysis;
}

// Main test
function main() {
    console.log('=== Adaptive SWAPPER_PG_DIR Discovery ===\n');

    const memoryPath = '/tmp/haywire-vm-mem';
    const fd = fs.openSync(memoryPath, 'r');
    const stats = fs.fstatSync(fd);
    const fileSize = stats.size;
    console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB`);

    // Detect actual RAM size from file
    const estimatedRamSize = Math.ceil((fileSize - 0x40000000) / (1024*1024*1024));
    console.log(`Estimated VM RAM size: ~${estimatedRamSize}GB\n`);

    console.log('Expected characteristics based on RAM size:');
    console.log(`- PGD[0] should have ~${estimatedRamSize} PUD entries for linear mapping`);
    console.log('- Sparse PGD (typically 2-10 total entries)');
    console.log('- Kernel entries at PGD[256] and/or PGD[500+]\n');

    // Scan regions - broader to catch different configurations
    const scanRegions = [
        [0x70000000, 0x80000000],   // 1.75-2GB
        [0x80000000, 0x140000000],  // 2-5GB range
        [0x130000000, 0x140000000], // 4.75-5GB range (includes known ground truth)
    ];

    const candidates = [];

    for (const [regionStart, regionEnd] of scanRegions) {
        const start = Math.max(0, regionStart - GUEST_RAM_START);
        const end = Math.min(fileSize, regionEnd - GUEST_RAM_START);

        if (start >= end) continue;

        console.log(`Scanning region 0x${regionStart.toString(16)}-0x${regionEnd.toString(16)}...`);

        let scanned = 0;
        for (let offset = start; offset < end; offset += PAGE_SIZE) {
            const physAddr = offset + GUEST_RAM_START;

            // Quick check for sparse page (optimization)
            const page = readPage(fd, offset);
            let nonZeroEntries = 0;
            for (let i = 0; i < 512; i++) {
                if (page.readBigUint64LE(i * 8) !== 0n) {
                    nonZeroEntries++;
                    if (nonZeroEntries > 20) break; // Too many entries
                }
            }

            // Only analyze sparse pages
            if (nonZeroEntries >= 2 && nonZeroEntries <= 20) {
                const analysis = analyzeSwapperCandidate(fd, fileSize, physAddr);
                if (analysis && analysis.score >= 3) {
                    candidates.push(analysis);
                    scanned++;
                }
            }
        }
        console.log(`  Scanned region, found ${scanned} candidates`);
    }

    // Sort by score
    candidates.sort((a, b) => b.score - a.score);

    console.log(`\nFound ${candidates.length} high-scoring candidates:\n`);

    // Show top candidates
    const top = candidates.slice(0, 10);
    for (const c of top) {
        console.log(c.summary);
        for (const reason of c.reasons) {
            console.log(`  + ${reason}`);
        }
        console.log('');
    }

    if (candidates.length > 0) {
        const best = candidates[0];
        console.log('=== BEST CANDIDATE ===');
        console.log(`Physical Address: 0x${best.physAddr.toString(16)}`);
        console.log(`Confidence Score: ${best.score}/7`);
        console.log(`Estimated RAM Size: ${best.memSizeEstimate}GB`);

        if (best.memSizeEstimate !== estimatedRamSize) {
            console.log(`\n⚠️  Note: PUD count (${best.memSizeEstimate}GB) differs from file size estimate (${estimatedRamSize}GB)`);
            console.log('This could indicate:');
            console.log('- Memory hotplug or ballooning');
            console.log('- Non-linear memory layout');
            console.log('- Huge pages in use');
        }
    }

    fs.closeSync(fd);
}

main();