#!/usr/bin/env node

import fs from 'fs';
import net from 'net';

const GUEST_RAM_START = 0x40000000;
const PAGE_SIZE = 4096;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;
const PA_MASK = 0x0000FFFFFFFFFFFFn;
const SWAPPER_PGD_PA = 0x136deb000;

// The init_task VA we found via kallsyms
const INIT_TASK_VA = 0xffff800083739840n;

console.log('=== Translating init_task VA to PA ===\n');
console.log(`init_task VA: 0x${INIT_TASK_VA.toString(16)}`);

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

function translateVA(fd, va, swapperPgd) {
    console.log(`\nTranslating VA 0x${va.toString(16)}...`);
    
    // Extract indices
    const pgdIndex = Number((va >> 39n) & 0x1FFn);
    const pudIndex = Number((va >> 30n) & 0x1FFn);
    const pmdIndex = Number((va >> 21n) & 0x1FFn);
    const pteIndex = Number((va >> 12n) & 0x1FFn);
    const pageOffset = Number(va & 0xFFFn);
    
    console.log(`  Indices: PGD[${pgdIndex}] PUD[${pudIndex}] PMD[${pmdIndex}] PTE[${pteIndex}] +0x${pageOffset.toString(16)}`);
    
    try {
        // Read PGD
        const pgdOffset = swapperPgd - GUEST_RAM_START;
        const pgdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pgdBuffer, 0, PAGE_SIZE, pgdOffset);
        
        const pgdEntry = pgdBuffer.readBigUint64LE(pgdIndex * 8);
        console.log(`  PGD[${pgdIndex}] = 0x${pgdEntry.toString(16)}`);
        
        if ((pgdEntry & 0x3n) === 0n) {
            console.log('  PGD entry is invalid!');
            return 0;
        }
        
        // Check for block mapping at PGD level (1GB)
        if ((pgdEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pgdEntry & PA_MASK & ~0x3FFFFFFFn);
            const pa = blockPA | (Number(va) & 0x3FFFFFFF);
            console.log(`  1GB block at PGD level -> PA 0x${pa.toString(16)}`);
            return pa;
        }
        
        // Read PUD
        const pudTablePA = Number(pgdEntry & PA_MASK & ~0xFFFn);
        console.log(`  PUD table at PA 0x${pudTablePA.toString(16)}`);
        const pudBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pudBuffer, 0, PAGE_SIZE, pudTablePA - GUEST_RAM_START);
        
        const pudEntry = pudBuffer.readBigUint64LE(pudIndex * 8);
        console.log(`  PUD[${pudIndex}] = 0x${pudEntry.toString(16)}`);
        
        if ((pudEntry & 0x3n) === 0n) {
            console.log('  PUD entry is invalid!');
            return 0;
        }
        
        // Check for block mapping at PUD level (2MB)
        if ((pudEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pudEntry & PA_MASK & ~0x1FFFFFn);
            const pa = blockPA | (Number(va) & 0x1FFFFF);
            console.log(`  2MB block at PUD level -> PA 0x${pa.toString(16)}`);
            return pa;
        }
        
        // Read PMD
        const pmdTablePA = Number(pudEntry & PA_MASK & ~0xFFFn);
        console.log(`  PMD table at PA 0x${pmdTablePA.toString(16)}`);
        const pmdBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pmdBuffer, 0, PAGE_SIZE, pmdTablePA - GUEST_RAM_START);
        
        const pmdEntry = pmdBuffer.readBigUint64LE(pmdIndex * 8);
        console.log(`  PMD[${pmdIndex}] = 0x${pmdEntry.toString(16)}`);
        
        if ((pmdEntry & 0x3n) === 0n) {
            console.log('  PMD entry is invalid!');
            return 0;
        }
        
        // Check for block mapping at PMD level (2MB)
        if ((pmdEntry & 0x3n) === 0x1n) {
            const blockPA = Number(pmdEntry & PA_MASK & ~0x1FFFFFn);
            const pa = blockPA | (Number(va) & 0x1FFFFF);
            console.log(`  2MB block at PMD level -> PA 0x${pa.toString(16)}`);
            return pa;
        }
        
        // Read PTE
        const pteTablePA = Number(pmdEntry & PA_MASK & ~0xFFFn);
        console.log(`  PTE table at PA 0x${pteTablePA.toString(16)}`);
        const pteBuffer = Buffer.allocUnsafe(PAGE_SIZE);
        fs.readSync(fd, pteBuffer, 0, PAGE_SIZE, pteTablePA - GUEST_RAM_START);
        
        const pteEntry = pteBuffer.readBigUint64LE(pteIndex * 8);
        console.log(`  PTE[${pteIndex}] = 0x${pteEntry.toString(16)}`);
        
        if ((pteEntry & 0x3n) === 0n) {
            console.log('  PTE entry is invalid!');
            return 0;
        }
        
        // Get final page
        const pagePA = Number(pteEntry & PA_MASK & ~0xFFFn);
        const finalPA = pagePA | pageOffset;
        console.log(`  Page at PA 0x${pagePA.toString(16)} + offset 0x${pageOffset.toString(16)}`);
        console.log(`  Final PA: 0x${finalPA.toString(16)}`);
        
        return finalPA;
        
    } catch (e) {
        console.log(`  Error during translation: ${e.message}`);
        return 0;
    }
}

// Get swapper_pg_dir from QMP if available
async function getSwapperPgd() {
    return new Promise((resolve) => {
        const socket = new net.Socket();
        let buffer = '';
        let capabilitiesSent = false;

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
                        socket.end();
                        resolve(Number(BigInt(msg.return.ttbr1) & PA_MASK));
                    }
                } catch (e) {}
            }
        });

        socket.on('close', () => resolve(SWAPPER_PGD_PA));
        socket.on('error', () => resolve(SWAPPER_PGD_PA));
        socket.connect(4445, 'localhost');
    });
}

async function main() {
    const swapperPgd = await getSwapperPgd();
    console.log(`Using swapper_pg_dir at PA 0x${swapperPgd.toString(16)}`);
    
    // Translate init_task VA to PA
    const initTaskPA = translateVA(fd, INIT_TASK_VA, swapperPgd);
    
    if (!initTaskPA) {
        console.log('\nFailed to translate init_task VA to PA');
        fs.closeSync(fd);
        return;
    }
    
    console.log(`\n=== INIT_TASK FOUND ===`);
    console.log(`VA: 0x${INIT_TASK_VA.toString(16)}`);
    console.log(`PA: 0x${initTaskPA.toString(16)}`);
    
    // Verify it's really init_task
    console.log('\nVerifying init_task contents...');
    
    const offset = initTaskPA - GUEST_RAM_START;
    
    try {
        // Read PID
        const pidBuffer = Buffer.allocUnsafe(4);
        fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
        const pid = pidBuffer.readUint32LE(0);
        
        // Read comm
        const commBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
        const comm = commBuffer.toString('ascii').split('\0')[0];
        
        // Read tasks list pointers
        const tasksBuffer = Buffer.allocUnsafe(16);
        fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
        const tasksNext = tasksBuffer.readBigUint64LE(0);
        const tasksPrev = tasksBuffer.readBigUint64LE(8);
        
        console.log(`  PID: ${pid}`);
        console.log(`  comm: "${comm}"`);
        console.log(`  tasks.next: 0x${tasksNext.toString(16)}`);
        console.log(`  tasks.prev: 0x${tasksPrev.toString(16)}`);
        
        if (pid === 0 && comm === 'swapper') {
            console.log('\n✅ CONFIRMED: This is the real init_task!');
            console.log('\nWe can now walk the complete process list from here!');
            console.log('tasks.next points to the first real process in the system.');
        } else {
            console.log('\n⚠️ Unexpected values - might need different field offsets');
        }
        
    } catch (e) {
        console.log(`Error reading init_task: ${e.message}`);
    }
    
    fs.closeSync(fd);
}

main().catch(console.error);