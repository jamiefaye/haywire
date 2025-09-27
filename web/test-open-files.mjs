#!/usr/bin/env node
/**
 * Test finding open files through process file tables
 * This builds on our working process discovery
 */

import fs from 'fs';
import net from 'net';

// Simple QMP connection
class QMPConnection {
    constructor(host = 'localhost', port = 4445) {
        this.host = host;
        this.port = port;
        this.socket = null;
    }

    async connect() {
        return new Promise((resolve, reject) => {
            this.socket = net.createConnection(this.port, this.host);

            this.socket.on('connect', () => {
                // Read banner
                this.socket.once('data', () => {
                    // Send capabilities
                    this.socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                    this.socket.once('data', () => {
                        resolve();
                    });
                });
            });

            this.socket.on('error', reject);
        });
    }

    async execute(command, args = {}) {
        return new Promise((resolve) => {
            const cmd = JSON.stringify({
                "execute": command,
                "arguments": args
            }) + '\n';

            this.socket.write(cmd);
            this.socket.once('data', (data) => {
                const response = JSON.parse(data.toString());
                resolve(response.return || response);
            });
        });
    }

    close() {
        if (this.socket) {
            this.socket.destroy();
        }
    }
}

// Memory reading via QMP
class MemoryReader {
    constructor(qmp) {
        this.qmp = qmp;
    }

    async readMemory(addr, size) {
        // Use our custom QMP command for kernel memory
        try {
            const result = await this.qmp.execute('query-cpu-memory', {
                'cpu-index': 0,
                'addr': addr,
                'size': size
            });

            if (result && result.memory) {
                // Convert hex string to buffer
                const hex = result.memory.replace(/\s/g, '');
                return Buffer.from(hex, 'hex');
            }
        } catch (e) {
            // Fallback to monitor command
        }

        // Fallback to human-monitor-command
        const result = await this.qmp.execute('human-monitor-command', {
            'command-line': `xp/${Math.ceil(size/8)}gx 0x${addr.toString(16)}`
        });

        // Parse the memory dump
        const bytes = [];
        const lines = result.split('\n');
        for (const line of lines) {
            // Match format: 0xaddr: 0x0123456789abcdef 0x0123456789abcdef
            const match = line.match(/0x[0-9a-f]+:\s+((?:0x[0-9a-f]+\s*)+)/);
            if (match) {
                const values = match[1].trim().split(/\s+/);
                for (const val of values) {
                    if (val.startsWith('0x')) {
                        const num = BigInt(val);
                        // Convert to little-endian bytes
                        for (let i = 0; i < 8; i++) {
                            bytes.push(Number((num >> BigInt(i * 8)) & 0xFFn));
                        }
                    }
                }
            }
        }

        return Buffer.from(bytes.slice(0, size));
    }

    async readU64(addr) {
        const data = await this.readMemory(addr, 8);
        if (data.length < 8) {
            console.error(`Warning: Got ${data.length} bytes instead of 8 at 0x${addr.toString(16)}`);
            return 0n;
        }
        return data.readBigUInt64LE();
    }

    async readU32(addr) {
        const data = await this.readMemory(addr, 4);
        return data.readUInt32LE();
    }
}

// Kernel offsets - these need to be determined for your kernel version
const OFFSETS = {
    // task_struct offsets
    'task_struct.files': 0x0,  // TODO: Find this offset
    'task_struct.comm': 0x758,  // Process name
    'task_struct.pid': 0x4E8,   // PID

    // files_struct offsets
    'files_struct.fdt': 0x20,   // File descriptor table pointer

    // fdtable offsets
    'fdtable.fd': 0x08,         // Array of file pointers
    'fdtable.max_fds': 0x00,    // Maximum number of fds

    // struct file offsets
    'file.f_inode': 0x20,       // Pointer to inode
    'file.f_path': 0x10,        // File path

    // inode offsets
    'inode.i_ino': 0x40,         // Inode number
    'inode.i_mode': 0x00,        // File mode
    'inode.i_size': 0x50,        // File size
    'inode.i_mapping': 0x30,     // Address space
};

async function findTaskStructFiles() {
    console.log('=== Finding Open Files Through Process File Tables ===\n');

    const qmp = new QMPConnection();
    await qmp.connect();
    console.log('Connected to QMP\n');

    const mem = new MemoryReader(qmp);

    // For testing, let's use a known task_struct address
    // First try to get init_task from kallsyms
    let INIT_TASK = 0xffff800083612800;  // Common init_task address

    try {
        const kallsyms = await mem.qmp.execute('human-monitor-command', {
            'command-line': 'info symbols init_task'
        });
        const match = kallsyms.match(/init_task.*0x([0-9a-f]+)/);
        if (match) {
            INIT_TASK = parseInt(match[1], 16);
            console.log(`Found init_task at 0x${INIT_TASK.toString(16)} from kallsyms`);
        }
    } catch (e) {
        console.log(`Using default init_task address: 0x${INIT_TASK.toString(16)}`);
    }

    try {
        console.log(`Reading task_struct at 0x${INIT_TASK.toString(16)}`);

        // First, let's try to find the files offset by scanning
        console.log('\nScanning for files_struct pointer pattern...');

        // Files_struct pointers are usually in the 0xffff0000xxxxxxxx range
        for (let offset = 0x500; offset < 0x800; offset += 8) {
            const ptr = await mem.readU64(INIT_TASK + offset);

            // Check if it looks like a kernel pointer
            if (ptr > 0xffff000000000000n && ptr < 0xffff800000000000n) {
                console.log(`  Offset 0x${offset.toString(16)}: 0x${ptr.toString(16)}`);

                // Try to validate if this could be files_struct
                // files_struct should have an atomic count as first field
                try {
                    const count = await mem.readU32(Number(ptr));
                    if (count > 0 && count < 100) {
                        console.log(`    -> Possible files_struct (count=${count})`);
                        OFFSETS['task_struct.files'] = offset;

                        // Try to read fdtable pointer
                        const fdtPtr = await mem.readU64(Number(ptr) + OFFSETS['files_struct.fdt']);
                        if (fdtPtr > 0xffff000000000000n) {
                            console.log(`    -> Has fdtable at 0x${fdtPtr.toString(16)}`);

                            // Try to read max_fds
                            const maxFds = await mem.readU32(Number(fdtPtr) + OFFSETS['fdtable.max_fds']);
                            console.log(`    -> Max FDs: ${maxFds}`);

                            if (maxFds > 0 && maxFds < 10000) {
                                console.log(`    ✓ This looks like a valid files_struct!`);
                                await examineOpenFiles(mem, Number(ptr));
                                break;
                            }
                        }
                    }
                } catch (e) {
                    // Not a valid pointer
                }
            }
        }

    } finally {
        qmp.close();
    }
}

async function examineOpenFiles(mem, filesStructAddr) {
    console.log(`\n=== Examining files_struct at 0x${filesStructAddr.toString(16)} ===\n`);

    // Read fdtable pointer
    const fdtPtr = await mem.readU64(filesStructAddr + OFFSETS['files_struct.fdt']);
    console.log(`FD table at: 0x${fdtPtr.toString(16)}`);

    // Read max_fds and fd array pointer
    const maxFds = await mem.readU32(Number(fdtPtr) + OFFSETS['fdtable.max_fds']);
    const fdArrayPtr = await mem.readU64(Number(fdtPtr) + OFFSETS['fdtable.fd']);

    console.log(`Max FDs: ${maxFds}`);
    console.log(`FD array at: 0x${fdArrayPtr.toString(16)}\n`);

    // Check first few file descriptors
    console.log('Open files:');
    for (let fd = 0; fd < Math.min(maxFds, 10); fd++) {
        const filePtr = await mem.readU64(Number(fdArrayPtr) + (fd * 8));

        if (filePtr && filePtr !== 0n) {
            console.log(`\n  FD ${fd}: file at 0x${filePtr.toString(16)}`);

            // Read inode pointer from file structure
            const inodePtr = await mem.readU64(Number(filePtr) + OFFSETS['file.f_inode']);
            if (inodePtr) {
                console.log(`    -> inode at 0x${inodePtr.toString(16)}`);

                // Read inode details
                const inoNum = await mem.readU64(Number(inodePtr) + OFFSETS['inode.i_ino']);
                const mode = await mem.readU32(Number(inodePtr) + OFFSETS['inode.i_mode']);
                const size = await mem.readU64(Number(inodePtr) + OFFSETS['inode.i_size']);

                console.log(`    -> inode #${inoNum}`);
                console.log(`    -> mode: 0x${mode.toString(16)} (${interpretFileMode(mode)})`);
                console.log(`    -> size: ${size} bytes`);

                // Read i_mapping for cache info
                const mappingPtr = await mem.readU64(Number(inodePtr) + OFFSETS['inode.i_mapping']);
                if (mappingPtr) {
                    console.log(`    -> i_mapping at 0x${mappingPtr.toString(16)}`);

                    // Try to read nrpages (cached pages count)
                    const nrpages = await mem.readU64(Number(mappingPtr) + 0x58);
                    if (nrpages < 1000000n) {  // Sanity check
                        console.log(`    -> ${nrpages} cached pages (${nrpages * 4096n / 1024n} KB)`);
                    }
                }
            }
        }
    }
}

function interpretFileMode(mode) {
    const types = {
        0x1000: 'FIFO',
        0x2000: 'CHR',
        0x4000: 'DIR',
        0x6000: 'BLK',
        0x8000: 'REG',
        0xA000: 'LNK',
        0xC000: 'SOCK'
    };

    const type = mode & 0xF000;
    const perms = (mode & 0x1FF).toString(8);

    return `${types[type] || 'UNKNOWN'} ${perms}`;
}

// Run the test
findTaskStructFiles().catch(console.error);