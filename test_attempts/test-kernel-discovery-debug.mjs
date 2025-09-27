#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const KNOWN_SWAPPER_PGD = 0x136DEB000;
const PA_MASK = 0x0000FFFFFFFFF000n;

// First, get kernel PGD from QMP
async function getKernelPGDFromQMP() {
    return new Promise((resolve, reject) => {
        const socket = new net.Socket();
        let buffer = '';
        let capabilitiesSent = false;

        socket.connect(4444, 'localhost', () => {
            console.log(`Connected to QMP at localhost:4444`);
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
                        socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                        capabilitiesSent = true;
                    } else if (capabilitiesSent && msg.return !== undefined && msg.return.ttbr1 === undefined) {
                        // Try query-kernel-info
                        socket.write(JSON.stringify({
                            "execute": "query-kernel-info",
                            "arguments": {"cpu-index": 0}
                        }) + '\n');
                    } else if (msg.return && msg.return.ttbr1) {
                        const ttbr1 = BigInt(msg.return.ttbr1);
                        const pgd = Number(ttbr1 & PA_MASK);
                        console.log(`✓ Got TTBR1_EL1 from QMP: 0x${ttbr1.toString(16)}`);
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
            resolve(null);  // Timeout, return null
        }, 3000);
    });
}

function examineKernelPGD(pgdPA) {
    const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
    const fileSize = fs.statSync('/tmp/haywire-vm-mem').size;

    // Calculate file offset
    const pgdFileOffset = pgdPA - GUEST_RAM_START;

    console.log(`Examining Kernel PGD:`);
    console.log(`  PA: 0x${pgdPA.toString(16)}`);
    console.log(`  File offset: 0x${pgdFileOffset.toString(16)}`);
    console.log(`  Known correct PA: 0x${KNOWN_SWAPPER_PGD.toString(16)}`);
    console.log(`  Match: ${pgdPA === KNOWN_SWAPPER_PGD ? '✓ YES' : '✗ NO'}\n`);

    if (pgdFileOffset < 0 || pgdFileOffset + 4096 > fileSize) {
        console.log('ERROR: PGD offset is outside file bounds!');
        fs.closeSync(fd);
        return;
    }

    // Read the PGD page
    const pgdBuffer = Buffer.allocUnsafe(4096);
    fs.readSync(fd, pgdBuffer, 0, 4096, pgdFileOffset);

    // Scan for valid entries
    let userEntries = 0;
    let kernelEntries = 0;
    const validEntries = [];

    for (let i = 0; i < 512; i++) {
        const entry = pgdBuffer.readBigUint64LE(i * 8);
        if (entry !== 0n) {
            const type = entry & 0x3n;
            if (type === 0x1n || type === 0x3n) {
                const physAddr = Number(entry & PA_MASK);
                validEntries.push({
                    index: i,
                    entry: entry,
                    type: type === 0x3n ? 'table' : 'block',
                    physAddr: physAddr
                });

                if (i < 256) userEntries++;
                else kernelEntries++;
            }
        }
    }

    console.log(`PGD Entry Summary:`);
    console.log(`  User entries (0-255): ${userEntries}`);
    console.log(`  Kernel entries (256-511): ${kernelEntries}`);
    console.log(`  Total valid entries: ${validEntries.length}`);

    console.log(`\nActual entries found:`);
    for (const e of validEntries) {
        const space = e.index < 256 ? 'USER  ' : 'KERNEL';
        console.log(`  [${e.index.toString().padStart(3)}] ${space}: 0x${e.entry.toString(16)} -> PA 0x${e.physAddr.toString(16)}`);
    }

    fs.closeSync(fd);
    return validEntries;
}

// Main
console.log('=== KERNEL PGD DISCOVERY DEBUG ===\n');

try {
    const qmpPgd = await getKernelPGDFromQMP();

    if (qmpPgd) {
        console.log('QMP provided kernel PGD. Examining it...\n');
        examineKernelPGD(qmpPgd);
    } else {
        console.log('QMP did not provide kernel PGD. Using known value...\n');
        examineKernelPGD(KNOWN_SWAPPER_PGD);
    }
} catch (err) {
    console.log('Error getting QMP PGD, using known value...\n');
    examineKernelPGD(KNOWN_SWAPPER_PGD);
}