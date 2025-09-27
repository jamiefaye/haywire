#!/usr/bin/env node

import net from 'net';
import fs from 'fs';

// Connect to QGA socket
async function runGuestCommand(command) {
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

                    // Skip the sync response
                    if (msg.return === 42 && !syncReceived) {
                        syncReceived = true;
                        // Now send the actual command
                        const cmd = {
                            execute: "guest-exec",
                            arguments: {
                                path: "/bin/sh",
                                arg: ["-c", command],
                                "capture-output": true
                            }
                        };
                        socket.write(JSON.stringify(cmd) + '\n');
                    } else if (msg.return && msg.return.pid !== undefined) {
                        // Got the exec response, now get the output
                        const pid = msg.return.pid;
                        setTimeout(() => {
                            const statusCmd = {
                                execute: "guest-exec-status",
                                arguments: { pid: pid }
                            };
                            socket.write(JSON.stringify(statusCmd) + '\n');
                        }, 500); // Wait for command to complete
                    } else if (msg.return && msg.return['out-data'] !== undefined) {
                        // Got the output
                        const output = Buffer.from(msg.return['out-data'], 'base64').toString();
                        socket.end();
                        resolve(output);
                    } else if (msg.return && msg.return.exited !== undefined && !msg.return['out-data']) {
                        // Command exited but no output
                        socket.end();
                        resolve('');
                    }
                } catch (e) {
                    // Ignore parse errors
                }
            }
        });

        socket.on('error', (err) => {
            reject(new Error(`Socket error: ${err.message}`));
        });

        socket.on('connect', () => {
            // Send sync first to clear any pending data
            socket.write(JSON.stringify({"execute": "guest-sync", "arguments": {"id": 42}}) + '\n');
        });

        // Connect to QGA socket (usually forwarded to a local port)
        // Try different common ports
        const ports = [8989, 7777, 4446];
        let connected = false;

        const tryConnect = (index) => {
            if (index >= ports.length) {
                // Try Unix socket as last resort
                socket.connect('/tmp/qga.sock', () => {
                    console.log('Connected to QGA via Unix socket');
                });
                return;
            }

            const port = ports[index];
            socket.connect(port, 'localhost', () => {
                console.log(`Connected to QGA on port ${port}`);
                connected = true;
            });

            socket.once('error', (err) => {
                if (!connected && err.code === 'ECONNREFUSED') {
                    // Try next port
                    socket.removeAllListeners();
                    tryConnect(index + 1);
                }
            });
        };

        tryConnect(0);
    });
}

async function getProcessListFromProc() {
    try {
        console.log('=== Getting Process List via /proc ===\n');

        // Method 1: List all numeric directories in /proc
        console.log('Method 1: Listing /proc directories...');
        const lsOutput = await runGuestCommand('ls -1 /proc | grep "^[0-9]*$" | sort -n');

        const pids = lsOutput.trim().split('\n').filter(line => line.match(/^\d+$/));
        console.log(`Found ${pids.length} processes\n`);

        // Method 2: Get process names for each PID
        console.log('Method 2: Getting process names...');
        const processInfo = [];

        // Batch the commands to avoid too many requests
        const batchSize = 50;
        for (let i = 0; i < pids.length; i += batchSize) {
            const batch = pids.slice(i, Math.min(i + batchSize, pids.length));

            // Build a command that gets PID and comm for all PIDs in batch
            const cmd = batch.map(pid =>
                `echo -n "${pid} "; cat /proc/${pid}/comm 2>/dev/null || echo "unknown"`
            ).join('; ');

            const output = await runGuestCommand(cmd);

            // Parse output
            const lines = output.trim().split('\n');
            for (const line of lines) {
                const parts = line.split(' ');
                if (parts.length >= 2) {
                    const pid = parseInt(parts[0]);
                    const comm = parts.slice(1).join(' ').trim();
                    if (!isNaN(pid)) {
                        processInfo.push({ pid, comm });
                    }
                }
            }

            console.log(`  Processed ${Math.min(i + batchSize, pids.length)}/${pids.length} PIDs...`);
        }

        // Sort by PID
        processInfo.sort((a, b) => a.pid - b.pid);

        // Display results
        console.log(`\n\nTotal processes found: ${processInfo.length}`);
        console.log('\nFirst 30 processes:');
        console.log('PID    COMMAND');
        console.log('---    -------');

        for (let i = 0; i < Math.min(30, processInfo.length); i++) {
            const p = processInfo[i];
            console.log(`${p.pid.toString().padEnd(6)} ${p.comm}`);
        }

        // Group by process name
        const byName = new Map();
        for (const p of processInfo) {
            const name = p.comm;
            if (!byName.has(name)) {
                byName.set(name, []);
            }
            byName.get(name).push(p.pid);
        }

        console.log('\n\nProcess count by name (top 20):');
        const sorted = Array.from(byName.entries()).sort((a, b) => b[1].length - a[1].length);
        for (const [name, pidList] of sorted.slice(0, 20)) {
            console.log(`  ${name}: ${pidList.length} process(es) - PIDs: ${pidList.slice(0, 5).join(', ')}${pidList.length > 5 ? '...' : ''}`);
        }

        // Save to file for comparison
        const fullOutput = processInfo.map(p => `${p.pid}\t${p.comm}`).join('\n');
        fs.writeFileSync('qga_proc_list.txt', fullOutput);
        console.log('\nFull process list saved to qga_proc_list.txt');

        // Also save just PIDs for easy comparison
        const pidList = processInfo.map(p => p.pid).join('\n');
        fs.writeFileSync('qga_pid_list.txt', pidList);
        console.log('PID list saved to qga_pid_list.txt');

        return processInfo;

    } catch (error) {
        console.error('Error:', error.message);
        console.log('\nMake sure:');
        console.log('1. QEMU guest agent is installed in the VM (qemu-guest-agent package)');
        console.log('2. Guest agent socket is configured in QEMU command line');
        console.log('3. Socket is accessible (forwarded port or Unix socket)');
        console.log('\nExample QEMU options:');
        console.log('  -chardev socket,path=/tmp/qga.sock,server=on,wait=off,id=qga0');
        console.log('  -device virtio-serial');
        console.log('  -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0');
    }
}

// Run the test
getProcessListFromProc().catch(console.error);