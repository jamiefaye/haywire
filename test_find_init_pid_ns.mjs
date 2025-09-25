#!/usr/bin/env node

import net from 'net';
import fs from 'fs';

const GUEST_RAM_START = 0x40000000;

console.log('=== Finding init_pid_ns via kallsyms ===\n');

// Get the address via kallsyms
async function getInitPidNsAddress() {
    return new Promise((resolve) => {
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
                                arg: ["-c", "grep ' init_pid_ns$' /proc/kallsyms | head -1"],
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
                        
                        // Parse: "ffff8000837354c0 D init_pid_ns"
                        const match = output.match(/([0-9a-f]+)\s+[DTRWVd]\s+init_pid_ns/);
                        if (match) {
                            resolve(`0x${match[1]}`);
                        } else {
                            resolve(null);
                        }
                    }
                } catch (e) {}
            }
        });

        socket.on('error', () => resolve(null));
        socket.on('connect', () => {
            socket.write(JSON.stringify({"execute": "guest-sync", "arguments": {"id": 42}}) + '\n');
        });

        socket.connect('/tmp/qga.sock');
    });
}

// Also try to get task_struct_cachep (the kmem_cache for task_structs)
async function getTaskStructCachep() {
    return new Promise((resolve) => {
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
                                arg: ["-c", "grep ' task_struct_cachep$' /proc/kallsyms | head -1"],
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
                        
                        const match = output.match(/([0-9a-f]+)\s+[DTRWVd]\s+task_struct_cachep/);
                        if (match) {
                            resolve(`0x${match[1]}`);
                        } else {
                            resolve(null);
                        }
                    }
                } catch (e) {}
            }
        });

        socket.on('error', () => resolve(null));
        socket.on('connect', () => {
            socket.write(JSON.stringify({"execute": "guest-sync", "arguments": {"id": 42}}) + '\n');
        });

        socket.connect('/tmp/qga.sock');
    });
}

async function main() {
    console.log('Querying kernel symbols via guest agent...\n');
    
    const initPidNsVA = await getInitPidNsAddress();
    if (initPidNsVA) {
        console.log(`init_pid_ns found at VA: ${initPidNsVA}`);
        console.log('This structure contains the IDR radix tree with ALL PIDs!');
        console.log('');
        console.log('struct pid_namespace init_pid_ns = {');
        console.log('    .idr = {            // <-- The IDR with all PIDs');
        console.log('        .idr_rt,        // Radix tree root');
        console.log('        .idr_base,      // Base ID');
        console.log('        .idr_next,      // Next ID to allocate');
        console.log('    },');
        console.log('    .pid_allocated,     // Number of PIDs allocated');
        console.log('    ...');
        console.log('};');
        console.log('');
        console.log('To enumerate ALL processes:');
        console.log('1. Translate init_pid_ns VA to PA');
        console.log('2. Parse the IDR radix tree structure');
        console.log('3. Extract every PID -> struct pid mapping');
        console.log('4. Get task_struct from each struct pid');
        console.log('');
        console.log('This is EXACTLY how /proc enumerates PIDs!');
    } else {
        console.log('Could not find init_pid_ns address');
    }
    
    console.log('\n' + '='.repeat(60) + '\n');
    
    const taskStructCachep = await getTaskStructCachep();
    if (taskStructCachep) {
        console.log(`task_struct_cachep found at VA: ${taskStructCachep}`);
        console.log('This is the kmem_cache for task_struct allocations!');
        console.log('');
        console.log('struct kmem_cache *task_struct_cachep points to:');
        console.log('    - All SLAB/SLUB metadata for task_structs');
        console.log('    - Per-CPU partial lists');
        console.log('    - Full slabs list');
        console.log('    - Free lists');
        console.log('');
        console.log('Following this would give us ALL task_structs,');
        console.log('including those in non-contiguous pages!');
    } else {
        console.log('Could not find task_struct_cachep address');
    }
    
    console.log('\n=== The Path to 100% Discovery ===\n');
    console.log('Option 1: Parse init_pid_ns.idr (radix tree)');
    console.log('  - Most reliable, exactly what /proc does');
    console.log('  - Complex radix tree parsing required');
    console.log('');
    console.log('Option 2: Walk task_struct_cachep slabs');
    console.log('  - Would find ALL allocated task_structs');
    console.log('  - Including those in non-contiguous pages');
    console.log('');
    console.log('Option 3: Fix the init_task linked list walk');
    console.log('  - Need to find the real init_task with valid pointers');
    console.log('  - Or understand why pointers appear broken');
    console.log('');
    console.log('Our current 91% is because we\'re only finding');
    console.log('task_structs in contiguous SLAB pages!');
}

main().catch(console.error);