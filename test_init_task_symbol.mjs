#!/usr/bin/env node

import net from 'net';
import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;
const TASKS_LIST_OFFSET = 0x7e0;

console.log('=== Finding init_task via Kernel Symbols ===\n');

// QMP connection to get kernel info
async function queryKernelSymbol(symbol) {
    return new Promise((resolve, reject) => {
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
                        // Try our custom command for kernel symbols
                        socket.write(JSON.stringify({
                            "execute": "query-kernel-symbol",
                            "arguments": {"symbol": symbol}
                        }) + '\n');
                    } else if (msg.return && msg.return.address !== undefined) {
                        socket.end();
                        resolve(msg.return);
                    } else if (msg.error) {
                        socket.end();
                        resolve(null);
                    }
                } catch (e) {}
            }
        });

        socket.on('close', () => resolve(null));
        socket.on('error', () => resolve(null));
        socket.connect(4445, 'localhost');

        setTimeout(() => {
            socket.end();
            resolve(null);
        }, 5000);
    });
}

// Try via /proc/kallsyms in the guest
async function getKallsymsViaGuest() {
    return new Promise((resolve, reject) => {
        const socket = new net.Socket();
        let buffer = '';
        let syncReceived = false;

        socket.on('data', (data) => {
            buffer += data.toString();
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (!line.trim()) continue;
                try {
                    const msg = JSON.parse(line);

                    if (msg.return === 42 && !syncReceived) {
                        syncReceived = true;
                        const cmd = {
                            execute: "guest-exec",
                            arguments: {
                                path: "/bin/sh",
                                arg: ["-c", "grep ' init_task$' /proc/kallsyms | head -1"],
                                "capture-output": true
                            }
                        };
                        socket.write(JSON.stringify(cmd) + '\n');
                    } else if (msg.return && msg.return.pid !== undefined) {
                        const pid = msg.return.pid;
                        setTimeout(() => {
                            const statusCmd = {
                                execute: "guest-exec-status",
                                arguments: { pid: pid }
                            };
                            socket.write(JSON.stringify(statusCmd) + '\n');
                        }, 500);
                    } else if (msg.return && msg.return['out-data'] !== undefined) {
                        const output = Buffer.from(msg.return['out-data'], 'base64').toString();
                        socket.end();
                        
                        // Parse kallsyms output: "ffff800011bf6a00 D init_task"
                        const match = output.match(/([0-9a-f]+)\s+[DTRWVd]\s+init_task/);
                        if (match) {
                            resolve(`0x${match[1]}`);
                        } else {
                            resolve(null);
                        }
                    } else if (msg.return && msg.return.exited !== undefined && !msg.return['out-data']) {
                        socket.end();
                        resolve(null);
                    }
                } catch (e) {}
            }
        });

        socket.on('error', () => resolve(null));
        socket.on('connect', () => {
            socket.write(JSON.stringify({"execute": "guest-sync", "arguments": {"id": 42}}) + '\n');
        });

        // Try Unix socket first
        socket.connect('/tmp/qga.sock', () => {
            console.log('Connected to QGA via Unix socket');
        });
    });
}

async function main() {
    // Method 1: Try QMP custom command (if QEMU has it)
    console.log('Method 1: Querying kernel symbol via QMP...');
    const qmpResult = await queryKernelSymbol('init_task');
    
    if (qmpResult) {
        console.log(`  Found via QMP: init_task at 0x${qmpResult.address.toString(16)}`);
    } else {
        console.log('  QMP query not available or failed');
    }

    // Method 2: Try via guest agent and kallsyms
    console.log('\nMethod 2: Checking /proc/kallsyms via guest agent...');
    const kallsymsAddr = await getKallsymsViaGuest();
    
    if (kallsymsAddr) {
        console.log(`  Found in kallsyms: init_task at ${kallsymsAddr}`);
        
        // This is a kernel virtual address - need to translate to PA
        const va = BigInt(kallsymsAddr);
        console.log('\n  This is a kernel VA. To convert to PA:');
        console.log('  1. Use swapper_pg_dir to walk page tables');
        console.log('  2. Or if in linear map, use simple translation');
        
        // Check if it's in the linear mapping range
        // ARM64 kernel linear map: VA 0xffff800000000000 maps to PA 0x40000000
        // Formula: PA = VA - 0xffff800000000000 + 0x40000000
        
        if (va >= 0xffff800000000000n && va < 0xffff800040000000n) {
            const pa = Number(va - 0xffff800000000000n) + GUEST_RAM_START;
            console.log(`\n  ✅ In linear map! PA = 0x${pa.toString(16)}`);
            
            // Verify by reading from memory file
            const memoryPath = '/tmp/haywire-vm-mem';
            const fd = fs.openSync(memoryPath, 'r');
            const offset = pa - GUEST_RAM_START;
            
            try {
                // Read PID
                const pidBuffer = Buffer.allocUnsafe(4);
                fs.readSync(fd, pidBuffer, 0, 4, offset + PID_OFFSET);
                const pid = pidBuffer.readUint32LE(0);
                
                // Read comm
                const commBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, commBuffer, 0, 16, offset + COMM_OFFSET);
                const comm = commBuffer.toString('ascii').split('\0')[0];
                
                // Read tasks pointers
                const tasksBuffer = Buffer.allocUnsafe(16);
                fs.readSync(fd, tasksBuffer, 0, 16, offset + TASKS_LIST_OFFSET);
                const tasksNext = tasksBuffer.readBigUint64LE(0);
                const tasksPrev = tasksBuffer.readBigUint64LE(8);
                
                console.log('\n  Verification at PA:');
                console.log(`    PID: ${pid}`);
                console.log(`    comm: "${comm}"`);
                console.log(`    tasks.next: 0x${tasksNext.toString(16)}`);
                console.log(`    tasks.prev: 0x${tasksPrev.toString(16)}`);
                
                if (pid === 0 && comm === 'swapper') {
                    console.log('\n  ✅ CONFIRMED: This is init_task!');
                    console.log(`\n=== INIT_TASK FOUND ===`);
                    console.log(`Kernel VA: ${kallsymsAddr}`);
                    console.log(`Physical Address: 0x${pa.toString(16)}`);
                    console.log(`\nYou can now walk the process list from here!`);
                } else {
                    console.log('\n  ⚠️  Unexpected values - might need different offsets');
                }
                
                fs.closeSync(fd);
            } catch (e) {
                console.log(`\n  Error reading from PA: ${e.message}`);
                fs.closeSync(fd);
            }
        } else {
            console.log(`\n  Not in linear map. Would need page table walk.`);
        }
    } else {
        console.log('  Could not get kallsyms (guest agent might not be available)');
    }

    // Method 3: Known addresses from kernel builds
    console.log('\nMethod 3: Common init_task addresses (kernel version dependent):');
    console.log('  Linux 6.x ARM64: often around 0xffff800011bf6a00');
    console.log('  Linux 5.x ARM64: often around 0xffff800011730000');
    console.log('  These vary by kernel config and version');
    
    console.log('\nNote: init_task location depends on:');
    console.log('  - Kernel version');
    console.log('  - Kernel config (KASLR, etc.)');
    console.log('  - Architecture');
    console.log('  - Compile-time layout');
}

main().catch(console.error);