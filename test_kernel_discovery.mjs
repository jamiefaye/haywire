#!/usr/bin/env node
/**
 * Test kernel discovery with improved swapper PGD scoring
 * This simulates what would happen in the web version
 */

import { readFileSync } from 'fs';
import { createConnection } from 'net';

// Simple QMP client to get ground truth
async function getQMPSwapperPGD() {
    return new Promise((resolve) => {
        const client = createConnection(4445, 'localhost');
        let buffer = '';

        client.on('connect', () => {
            // Consume banner
            client.once('data', () => {
                // Send capabilities
                client.write(JSON.stringify({execute: 'qmp_capabilities'}) + '\n');

                client.once('data', () => {
                    // Query kernel info
                    client.write(JSON.stringify({
                        execute: 'query-kernel-info',
                        arguments: {'cpu-index': 0}
                    }) + '\n');
                });
            });
        });

        client.on('data', (data) => {
            buffer += data.toString();
            const lines = buffer.split('\n');

            for (const line of lines) {
                if (line.includes('swapper-pg-dir')) {
                    try {
                        const response = JSON.parse(line);
                        if (response.return && response.return['swapper-pg-dir']) {
                            const pgd = response.return['swapper-pg-dir'];
                            client.end();
                            resolve(pgd);
                            return;
                        }
                    } catch (e) {
                        // Not valid JSON yet
                    }
                }
            }
        });

        client.on('error', () => {
            resolve(0);
        });

        setTimeout(() => {
            client.end();
            resolve(0);
        }, 2000);
    });
}

// Improved scoring algorithm
function analyzeSwapperCandidate(buffer, offset) {
    const analysis = {
        physAddr: offset + 0x40000000,
        score: 0,
        reasons: [],
        userEntries: 0,
        kernelEntries: 0
    };

    // Count entries
    for (let i = 0; i < 512; i++) {
        const entryOffset = offset + (i * 8);
        if (entryOffset + 8 > buffer.length) break;

        const entry = buffer.readBigUInt64LE(entryOffset);
        if (entry === 0n) continue;

        const entryType = Number(entry) & 0x3;
        if (entryType !== 0x1 && entryType !== 0x3) continue;

        if (i < 256) {
            analysis.userEntries++;
        } else {
            analysis.kernelEntries++;
        }
    }

    // CRITICAL: Swapper has VERY FEW user entries
    if (analysis.userEntries === 1) {
        analysis.score += 100;
        analysis.reasons.push('Single user entry (swapper!)');
    } else if (analysis.userEntries === 2) {
        analysis.score += 50;
        analysis.reasons.push('Two user entries');
    } else if (analysis.userEntries <= 4) {
        analysis.score += 20;
        analysis.reasons.push(`${analysis.userEntries} user entries`);
    } else if (analysis.userEntries <= 16) {
        analysis.score += 5;
    } else {
        analysis.score -= 50;
        analysis.reasons.push('Too many user entries');
    }

    // Check for kernel text at PGD[256]
    const pgd256 = buffer.readBigUInt64LE(offset + (256 * 8));
    if (pgd256 !== 0n && (Number(pgd256) & 0x3) !== 0) {
        analysis.score += 15;
        analysis.reasons.push('Has PGD[256]');
    } else {
        analysis.score -= 20;
    }

    // Kernel entry count
    if (analysis.kernelEntries >= 2 && analysis.kernelEntries <= 20) {
        analysis.score += 20;
        analysis.reasons.push(`${analysis.kernelEntries} kernel entries`);
    }

    return analysis;
}

// Find swapper PGD using improved algorithm
function findSwapperBySignature(buffer) {
    const candidates = [];

    // Scan ranges
    const ranges = [
        [0xf0000000, 0x100000000],   // 3.75-4GB
        [0x130000000, 0x140000000],  // 4.75-5GB (highmem)
        [0x70000000, 0x80000000]     // 1.75-2GB (highmem=off)
    ];

    for (const [start, end] of ranges) {
        if (start < 0 || end > buffer.length) continue;

        for (let offset = start; offset < Math.min(end, buffer.length); offset += 0x1000) {
            if (offset + 0x1000 > buffer.length) continue;

            // Quick filter
            const first = buffer.readBigUInt64LE(offset);
            if (first === 0n || (Number(first) & 0x3) === 0) continue;

            const analysis = analyzeSwapperCandidate(buffer, offset);
            if (analysis.score > 0) {
                candidates.push(analysis);
            }
        }
    }

    // Sort by score
    candidates.sort((a, b) => b.score - a.score);

    return candidates;
}

async function main() {
    console.log('=== Testing Improved Swapper PGD Discovery ===\n');

    // Get QMP ground truth
    const qmpPGD = await getQMPSwapperPGD();
    console.log(`QMP Ground Truth: 0x${qmpPGD.toString(16)}\n`);

    // Read memory file
    console.log('Reading memory file...');
    const buffer = readFileSync('/tmp/haywire-vm-mem');
    console.log(`Memory size: ${buffer.length / (1024*1024*1024)}GB\n`);

    // Find candidates with improved algorithm
    console.log('Running signature search with improved scoring...');
    const candidates = findSwapperBySignature(buffer);

    console.log(`\nFound ${candidates.length} candidates\n`);
    console.log('Top 5 candidates:');
    console.log('Rank |     Address | Score | User | Kernel | Match | Reasons');
    console.log('-----|-------------|-------|------|--------|-------|--------');

    for (let i = 0; i < Math.min(5, candidates.length); i++) {
        const c = candidates[i];
        const match = c.physAddr === qmpPGD ? '✅ QMP' : '      ';
        console.log(`  ${i+1}  | 0x${c.physAddr.toString(16).padStart(9, '0')} | ${c.score.toString().padStart(5)} | ${c.userEntries.toString().padStart(4)} | ${c.kernelEntries.toString().padStart(6)} | ${match} | ${c.reasons.join(', ')}`);
    }

    if (candidates.length > 0) {
        const best = candidates[0];
        console.log(`\nBest candidate: 0x${best.physAddr.toString(16)}`);

        if (best.physAddr === qmpPGD) {
            console.log('✅ SUCCESS! Signature search matches QMP ground truth!');
        } else {
            const diff = Math.abs(best.physAddr - qmpPGD);
            console.log(`❌ MISMATCH! Off by 0x${diff.toString(16)} (${diff/0x1000} pages)`);

            // Find where QMP ranks
            const qmpRank = candidates.findIndex(c => c.physAddr === qmpPGD) + 1;
            if (qmpRank > 0) {
                console.log(`   QMP candidate ranked #${qmpRank}`);
            }
        }
    }

    console.log('\n=== Reliability Assessment ===');
    if (candidates[0]?.physAddr === qmpPGD) {
        console.log('✅ Improved algorithm is RELIABLE for this VM configuration');
    } else {
        console.log('⚠️  Algorithm needs further tuning for this configuration');
    }
}

main().catch(console.error);