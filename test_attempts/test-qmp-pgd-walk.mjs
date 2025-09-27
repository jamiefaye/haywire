#!/usr/bin/env node

import net from 'net';
import fs from 'fs';

const QMP_HOST = 'localhost';
const QMP_PORT = 4445;
const GUEST_RAM_START = 0x40000000;
const PA_MASK = 0x0000FFFFFFFFF000n;

async function getKernelPGDFromQMP() {
    return new Promise((resolve, reject) => {
        const socket = new net.Socket();
        let buffer = '';
        let capabilitiesSent = false;

        socket.connect(QMP_PORT, QMP_HOST, () => {
            console.log(`Connected to QMP at ${QMP_HOST}:${QMP_PORT}`);
        });

        socket.on('data', (data) => {
            buffer += data.toString();
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (!line.trim()) continue;
                
                try {
                    const msg = JSON.parse(line);
                    
                    if (msg.QMP) {
                        console.log('Sending QMP capabilities...');
                        socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                        capabilitiesSent = true;
                    } else if (capabilitiesSent && msg.return !== undefined && msg.return.ttbr1 === undefined) {
                        // Try query-kernel-info
                        console.log('Querying kernel info...');
                        socket.write(JSON.stringify({
                            "execute": "query-kernel-info",
                            "arguments": {"cpu-index": 0}
                        }) + '\n');
                    } else if (msg.return && msg.return.ttbr1) {
                        // Got kernel info
                        const ttbr1 = BigInt(msg.return.ttbr1);
                        const pgd = Number(ttbr1 & PA_MASK);
                        console.log(`\n✓ Got TTBR1_EL1 from QMP: 0x${ttbr1.toString(16)}`);
                        console.log(`  Kernel PGD PA: 0x${pgd.toString(16)}\n`);
                        socket.end();
                        resolve(pgd);
                    } else if (msg.error) {
                        console.log('QMP Error:', msg.error.desc);
                        socket.end();
                        resolve(null);
                    }
                } catch (e) {
                    // Not JSON
                }
            }
        });

        socket.on('error', (err) => {
            console.error('QMP connection error:', err.message);
            reject(err);
        });

        setTimeout(() => {
            socket.end();
            reject(new Error('QMP timeout'));
        }, 5000);
    });
}

function walkPGD(pgdPA) {
    const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
    const fileSize = fs.statSync('/tmp/haywire-vm-mem').size;
    
    console.log(`Walking PGD at PA 0x${pgdPA.toString(16)}`);
    console.log('=' .repeat(60));
    
    // Read PGD
    const pgdOffset = pgdPA - GUEST_RAM_START;
    console.log(`  File offset: 0x${pgdOffset.toString(16)}`);
    
    if (pgdOffset < 0 || pgdOffset + 4096 > fileSize) {
        console.log('  ERROR: PGD offset out of bounds!');
        fs.closeSync(fd);
        return;
    }
    
    const pgdBuffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, pgdBuffer, 0, 4096, pgdOffset);
    
    let userEntries = 0;
    let kernelEntries = 0;
    const validEntries = [];
    
    // Scan all 512 entries
    for (let i = 0; i < 512; i++) {
        const entry = pgdBuffer.readBigUint64LE(i * 8);
        if (entry !== 0n) {
            const type = entry & 0x3n;
            if (type === 0x3n || type === 0x1n) {  // Valid table or block
                const entryInfo = {
                    idx: i,
                    entry: entry,
                    type: type === 0x3n ? 'table' : 'block',
                    pa: Number(entry & PA_MASK)
                };
                validEntries.push(entryInfo);
                
                if (i < 256) {
                    userEntries++;
                } else {
                    kernelEntries++;
                }
            }
        }
    }
    
    console.log(`\nPGD Summary:`);
    console.log(`  User entries (0-255): ${userEntries}`);
    console.log(`  Kernel entries (256-511): ${kernelEntries}`);
    console.log(`  Total valid entries: ${validEntries.length}`);
    
    // Show all entries with details
    console.log(`\nPGD Entries:`);
    for (const e of validEntries) {
        const space = e.idx < 256 ? 'USER' : 'KERNEL';
        console.log(`  [${e.idx}] ${space}: 0x${e.entry.toString(16)} (${e.type}) -> PA 0x${e.pa.toString(16)}`);
        
        // Try to read the next level (PUD)
        if (e.type === 'table' && e.pa < fileSize) {
            const pudBuffer = Buffer.allocUnsafe(64);  // Just check first few entries
            fs.readSync(fd, pudBuffer, 0, 64, e.pa);
            
            let pudValid = 0;
            for (let j = 0; j < 8; j++) {
                const pudEntry = pudBuffer.readBigUint64LE(j * 8);
                if (pudEntry !== 0n && (pudEntry & 0x3n) >= 1) {
                    pudValid++;
                }
            }
            console.log(`    -> PUD at PA 0x${e.pa.toString(16)}: ${pudValid}/8 valid entries in first 8`);
        }
    }
    
    // Try walking for kernel PTEs
    console.log(`\nAttempting to walk kernel entries for PTEs:`);
    let pteCount = 0;
    
    for (const e of validEntries) {
        if (e.idx >= 256 && e.type === 'table') {  // Kernel space table entry
            console.log(`\n  Walking kernel PGD[${e.idx}] -> PUD at 0x${e.pa.toString(16)}`);
            
            // Walk PUD table
            const pudBuffer = Buffer.allocUnsafe(4096);
            if (e.pa < fileSize) {
                fs.readSync(fd, pudBuffer, 0, 4096, e.pa);
                
                for (let pudIdx = 0; pudIdx < 512; pudIdx++) {
                    const pudEntry = pudBuffer.readBigUint64LE(pudIdx * 8);
                    if (pudEntry !== 0n && (pudEntry & 0x3n) === 0x3n) {  // Table descriptor
                        const pmdPA = Number(pudEntry & PA_MASK);
                        if (pmdPA < fileSize) {
                            // Found PMD table
                            const pmdBuffer = Buffer.allocUnsafe(4096);
                            fs.readSync(fd, pmdBuffer, 0, 4096, pmdPA);
                            
                            for (let pmdIdx = 0; pmdIdx < 512; pmdIdx++) {
                                const pmdEntry = pmdBuffer.readBigUint64LE(pmdIdx * 8);
                                if (pmdEntry !== 0n) {
                                    const pmdType = pmdEntry & 0x3n;
                                    if (pmdType === 0x1n) {  // 2MB block
                                        pteCount++;
                                        if (pteCount <= 3) {
                                            const blockPA = Number(pmdEntry & PA_MASK);
                                            const va = (BigInt(e.idx) << 39n) | (BigInt(pudIdx) << 30n) | (BigInt(pmdIdx) << 21n);
                                            console.log(`    Found 2MB block: VA 0x${va.toString(16)} -> PA 0x${blockPA.toString(16)}`);
                                        }
                                    } else if (pmdType === 0x3n) {  // PTE table
                                        const ptePA = Number(pmdEntry & PA_MASK);
                                        if (ptePA < fileSize) {
                                            const pteBuffer = Buffer.allocUnsafe(4096);
                                            fs.readSync(fd, pteBuffer, 0, 4096, ptePA);
                                            
                                            for (let pteIdx = 0; pteIdx < 512; pteIdx++) {
                                                const pteEntry = pteBuffer.readBigUint64LE(pteIdx * 8);
                                                if (pteEntry !== 0n && (pteEntry & 0x3n) === 0x3n) {
                                                    pteCount++;
                                                    if (pteCount <= 3) {
                                                        const pagePA = Number(pteEntry & PA_MASK);
                                                        const va = (BigInt(e.idx) << 39n) | (BigInt(pudIdx) << 30n) | 
                                                                  (BigInt(pmdIdx) << 21n) | (BigInt(pteIdx) << 12n);
                                                        console.log(`    Found 4KB page: VA 0x${va.toString(16)} -> PA 0x${pagePA.toString(16)}`);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else if (pudEntry !== 0n && (pudEntry & 0x3n) === 0x1n) {  // 1GB block
                        pteCount++;
                        if (pteCount <= 3) {
                            const blockPA = Number(pudEntry & PA_MASK);
                            const va = (BigInt(e.idx) << 39n) | (BigInt(pudIdx) << 30n);
                            console.log(`    Found 1GB block: VA 0x${va.toString(16)} -> PA 0x${blockPA.toString(16)}`);
                        }
                    }
                }
            }
        }
    }
    
    console.log(`\nTotal PTEs found: ${pteCount}`);
    
    fs.closeSync(fd);
}

// Main
console.log('Getting kernel PGD from QMP and walking it...\n');

getKernelPGDFromQMP()
    .then(pgd => {
        if (pgd) {
            walkPGD(pgd);
        } else {
            console.log('\nFailed to get kernel PGD from QMP.');
            console.log('QMP query-kernel-info might not be implemented in your QEMU.');
            console.log('\nTrying candidate with correct signature instead...');
            walkPGD(0x43f50000);  // The one with 1 user, 2 kernel entries
        }
    })
    .catch(err => {
        console.error('Error:', err.message);
        console.log('\nTrying candidate with correct signature instead...');
        walkPGD(0x43f50000);  // Fallback to our discovered candidate
    });