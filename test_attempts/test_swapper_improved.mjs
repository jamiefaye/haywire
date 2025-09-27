#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

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

// Check full PGD signature - both user and kernel entries
function analyzeFullPgdSignature(fd, fileSize, physAddr) {
    const offset = physAddr - GUEST_RAM_START;
    if (offset < 0 || offset + PAGE_SIZE > fileSize) {
        return null;
    }

    const page = readPage(fd, offset);
    const analysis = {
        physAddr,
        userEntries: [],    // Entries 0-255
        kernelEntries: [],  // Entries 256-511
        totalValid: 0,
        pgd0Details: null
    };

    // Scan all 512 entries
    for (let i = 0; i < 512; i++) {
        const entry = page.readBigUint64LE(i * 8);
        const entryType = Number(entry & 0x3n);

        if (entryType === 0x1 || entryType === 0x3) {
            analysis.totalValid++;
            const entryInfo = {
                index: i,
                entry,
                type: entryType === 0x1 ? 'BLOCK' : 'TABLE',
                pa: Number(entry & PA_MASK)
            };

            if (i < 256) {
                analysis.userEntries.push(entryInfo);
            } else {
                analysis.kernelEntries.push(entryInfo);
            }
        }
    }

    // Special analysis of PGD[0] if it exists
    const pgd0 = page.readBigUint64LE(0);
    if (pgd0 && (pgd0 & 0x3n)) {
        const pgd0Type = Number(pgd0 & 0x3n);
        analysis.pgd0Details = {
            entry: pgd0,
            type: pgd0Type === 0x1 ? 'BLOCK' : 'TABLE',
            pa: Number(pgd0 & PA_MASK)
        };

        // If PGD[0] is a table, analyze its PUD entries
        if (pgd0Type === 0x3) {
            const pudOffset = analysis.pgd0Details.pa - GUEST_RAM_START;
            if (pudOffset >= 0 && pudOffset + PAGE_SIZE <= fileSize) {
                const pudPage = readPage(fd, pudOffset);
                let validPuds = 0;
                const pudDetails = [];

                for (let j = 0; j < 512; j++) {
                    const pud = pudPage.readBigUint64LE(j * 8);
                    const pudType = Number(pud & 0x3n);
                    if (pudType >= 1) {
                        validPuds++;
                        if (pudDetails.length < 10) {
                            pudDetails.push({
                                index: j,
                                type: pudType === 0x1 ? 'BLOCK' : 'TABLE',
                                pa: Number(pud & PA_MASK)
                            });
                        }
                    }
                }

                analysis.pgd0Details.pudCount = validPuds;
                analysis.pgd0Details.pudSamples = pudDetails;
            }
        }
    }

    return analysis;
}

// Check if this looks like swapper_pg_dir based on full analysis
function isLikelySwapperPgDir(analysis) {
    if (!analysis) return false;

    // Must have PGD[0] entry
    if (!analysis.pgd0Details) return false;

    // Refined patterns based on ground truth and expected kernel behavior:

    // 1. Very few total entries (typically 2-6)
    // Ground truth has 4: PGD[0], PGD[256], PGD[507], PGD[511]
    if (analysis.totalValid < 2 || analysis.totalValid > 8) return false;

    // 2. Must have at least 1 user entry (PGD[0]) and at least 1 kernel entry
    if (analysis.userEntries.length < 1 || analysis.kernelEntries.length < 1) return false;

    // 3. PGD[0] should have specific PUD patterns:
    if (analysis.pgd0Details.type === 'TABLE') {
        const pudCount = analysis.pgd0Details.pudCount || 0;

        // Either exactly 4 PUDs (linear mapping of first 4GB)
        // Or 0 PUDs (special case we saw)
        // Or 13 PUDs (another pattern we found)
        if (pudCount !== 0 && pudCount !== 4 && pudCount !== 13) {
            return false;
        }

        // If we have 4 PUDs, they should be the first 4 (indices 0-3)
        if (pudCount === 4 && analysis.pgd0Details.pudSamples) {
            const indices = analysis.pgd0Details.pudSamples.map(p => p.index);
            const expectedFirst4 = indices.filter(i => i < 4).length === 4;
            if (!expectedFirst4) return false;
        }
    }

    // 4. Kernel entries should be in expected ranges
    // Common kernel PGD indices: 256 (start of kernel space),
    // 507-511 (fixmap, modules, etc)
    let hasKernelStart = false;
    let hasHighKernel = false;

    for (const entry of analysis.kernelEntries) {
        // Accept 256 (kernel text), or high entries (500+)
        if (entry.index !== 256 && entry.index < 500) {
            return false; // Unusual kernel entry location
        }
        if (entry.index === 256) hasKernelStart = true;
        if (entry.index >= 507) hasHighKernel = true;
    }

    // 5. Should have exactly 1 entry in user space (just PGD[0])
    // User processes have multiple user entries
    if (analysis.userEntries.length > 2) return false;

    // 6. Additional scoring for likely patterns
    // The real swapper_pg_dir typically has:
    // - PGD[256] (kernel start) AND high entries (507-511)
    // - Exactly 4 PUD entries in PGD[0]
    let score = 0;
    if (hasKernelStart && hasHighKernel) score += 2;  // Both kernel regions
    if (analysis.pgd0Details.type === 'TABLE' && analysis.pgd0Details.pudCount === 4) score += 2;
    if (analysis.userEntries.length === 1) score += 1;  // Exactly 1 user entry
    if (analysis.kernelEntries.length >= 2) score += 1;  // Multiple kernel entries

    // Store score for ranking
    analysis.score = score;

    return true;
}

// Scan for swapper_pg_dir candidates
function findSwapperCandidates(fd, fileSize) {
    console.log('\nScanning for swapper_pg_dir candidates...\n');

    const scanRegions = [
        [0x70000000, 0x80000000],   // 1.75-2GB
        [0x130000000, 0x140000000], // 4.75-5GB range
    ];

    const candidates = [];

    for (const [regionStart, regionEnd] of scanRegions) {
        const start = Math.max(0, regionStart - GUEST_RAM_START);
        const end = Math.min(fileSize, regionEnd - GUEST_RAM_START);

        if (start >= end) continue;

        console.log(`Scanning region 0x${regionStart.toString(16)}-0x${regionEnd.toString(16)}...`);

        for (let offset = start; offset < end; offset += PAGE_SIZE) {
            const physAddr = offset + GUEST_RAM_START;
            const analysis = analyzeFullPgdSignature(fd, fileSize, physAddr);

            if (isLikelySwapperPgDir(analysis)) {
                candidates.push(analysis);
                console.log(`  Found candidate at 0x${physAddr.toString(16)} (score: ${analysis.score}/6)`);
                console.log(`    User entries: ${analysis.userEntries.length}, Kernel entries: ${analysis.kernelEntries.length}`);
                if (analysis.pgd0Details) {
                    console.log(`    PGD[0]: ${analysis.pgd0Details.type}`);
                    if (analysis.pgd0Details.pudCount !== undefined) {
                        console.log(`      -> ${analysis.pgd0Details.pudCount} valid PUD entries`);
                        if (analysis.pgd0Details.pudSamples.length > 0) {
                            const samples = analysis.pgd0Details.pudSamples.slice(0, 4);
                            console.log(`      -> First PUDs: ${samples.map(p => `[${p.index}]`).join(', ')}`);
                        }
                    } else if (analysis.pgd0Details.type === 'TABLE') {
                        // PUD table wasn't analyzed - likely outside range or zero
                        console.log(`      -> PUD table at 0x${analysis.pgd0Details.pa.toString(16)} (not analyzed)`);
                    }
                }
                // Show kernel entries if any
                if (analysis.kernelEntries.length > 0) {
                    const indices = analysis.kernelEntries.map(e => e.index);
                    console.log(`    Kernel PGD entries at: ${indices.join(', ')}`);
                }
            }
        }
    }

    return candidates;
}

// Get ground truth from QMP
async function getGroundTruthFromQMP() {
    return new Promise((resolve, reject) => {
        const socket = new net.Socket();
        let buffer = '';
        let capabilitiesSent = false;
        let kernelInfo = null;

        socket.on('data', (data) => {
            buffer += data.toString();
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (!line.trim()) continue;
                try {
                    const msg = JSON.parse(line);
                    if (msg.QMP) {
                        socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                    } else if (msg.return !== undefined && !capabilitiesSent) {
                        capabilitiesSent = true;
                        socket.write(JSON.stringify({
                            "execute": "query-kernel-info",
                            "arguments": {"cpu-index": 0}
                        }) + '\n');
                    } else if (msg.return && msg.return.ttbr1 !== undefined) {
                        kernelInfo = msg.return;
                        socket.end();
                    }
                } catch (e) {
                    // Ignore
                }
            }
        });

        socket.on('close', () => {
            if (kernelInfo) {
                resolve(kernelInfo);
            } else {
                reject(new Error('Failed to get kernel info from QMP'));
            }
        });

        socket.on('error', reject);
        socket.connect(4445, 'localhost');
    });
}

// Main test
async function main() {
    console.log('=== Improved SWAPPER_PG_DIR Discovery Test ===\n');

    try {
        // Get ground truth
        const kernelInfo = await getGroundTruthFromQMP();
        const groundTruthPgd = Number(BigInt(kernelInfo.ttbr1) & PA_MASK);
        console.log(`Ground truth swapper_pg_dir (from QMP): 0x${groundTruthPgd.toString(16)}`);

        // Open memory file
        const memoryPath = '/tmp/haywire-vm-mem';
        const fd = fs.openSync(memoryPath, 'r');
        const stats = fs.fstatSync(fd);
        const fileSize = stats.size;
        console.log(`Memory file size: ${(fileSize / (1024*1024*1024)).toFixed(2)}GB`);

        // Analyze ground truth
        console.log('\n=== Ground Truth Analysis ===');
        const gtAnalysis = analyzeFullPgdSignature(fd, fileSize, groundTruthPgd);
        if (gtAnalysis) {
            console.log(`Total valid entries: ${gtAnalysis.totalValid}`);
            console.log(`User space entries (0-255): ${gtAnalysis.userEntries.length}`);
            console.log(`Kernel space entries (256-511): ${gtAnalysis.kernelEntries.length}`);

            if (gtAnalysis.pgd0Details) {
                console.log(`\nPGD[0] details:`);
                console.log(`  Type: ${gtAnalysis.pgd0Details.type}`);
                console.log(`  PA: 0x${gtAnalysis.pgd0Details.pa.toString(16)}`);
                if (gtAnalysis.pgd0Details.pudCount !== undefined) {
                    console.log(`  PUD entries: ${gtAnalysis.pgd0Details.pudCount}`);
                    if (gtAnalysis.pgd0Details.pudSamples.length > 0) {
                        console.log(`  Sample PUD indices:`);
                        for (const pud of gtAnalysis.pgd0Details.pudSamples) {
                            console.log(`    PUD[${pud.index}]: ${pud.type} -> 0x${pud.pa.toString(16)}`);
                        }
                    }
                }
            }

            // Show all entries
            console.log('\nAll valid PGD entries:');
            for (const e of gtAnalysis.userEntries) {
                console.log(`  PGD[${e.index}]: ${e.type} -> 0x${e.pa.toString(16)}`);
            }
            for (const e of gtAnalysis.kernelEntries) {
                console.log(`  PGD[${e.index}]: ${e.type} -> 0x${e.pa.toString(16)}`);
            }

            console.log(`\nWould our heuristic identify it? ${isLikelySwapperPgDir(gtAnalysis) ? 'YES' : 'NO'}`);
        }

        // Find candidates
        const candidates = findSwapperCandidates(fd, fileSize);

        // Sort by score
        candidates.sort((a, b) => b.score - a.score);

        // Check results
        console.log('\n=== RESULTS ===');
        console.log(`Found ${candidates.length} candidates (sorted by score):`);
        for (const c of candidates) {
            const isGT = c.physAddr === groundTruthPgd ? ' ← GROUND TRUTH' : '';
            console.log(`  0x${c.physAddr.toString(16)} (score: ${c.score}/6)${isGT}`);
        }

        const groundTruthFound = candidates.find(c => c.physAddr === groundTruthPgd);
        if (groundTruthFound) {
            const rank = candidates.indexOf(groundTruthFound) + 1;
            console.log(`\n✅ SUCCESS! Ground truth found at rank ${rank}/${candidates.length}`);
            if (rank === 1) {
                console.log('🎯 Ground truth has the highest score!');
            }
        } else {
            console.log('\n❌ FAILED! Ground truth not found');
        }

        fs.closeSync(fd);

    } catch (error) {
        console.error('Error:', error.message);
    }
}

main().catch(console.error);