#!/usr/bin/env node

import net from 'net';

console.log('Testing QMP connection from Node.js...\n');

function testQMP() {
    return new Promise((resolve) => {
        const socket = new net.Socket();
        let buffer = '';
        let capabilitiesSent = false;
        let responded = false;

        const cleanup = () => {
            if (!socket.destroyed) {
                socket.destroy();
            }
        };

        socket.connect(4445, 'localhost', () => {
            console.log('✓ Connected to QMP at localhost:4445');
        });

        socket.on('data', (data) => {
            buffer += data.toString();
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (!line.trim() || responded) continue;

                try {
                    const msg = JSON.parse(line);

                    if (msg.QMP && !capabilitiesSent) {
                        console.log('✓ Received QMP banner, sending capabilities...');
                        socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                        capabilitiesSent = true;
                    } else if (capabilitiesSent && msg.return !== undefined && !msg.return.ttbr1) {
                        console.log('✓ Capabilities acknowledged, querying kernel info...');
                        socket.write(JSON.stringify({
                            "execute": "query-kernel-info",
                            "arguments": {"cpu-index": 0}
                        }) + '\n');
                    } else if (msg.return && msg.return.ttbr1) {
                        responded = true;
                        const ttbr1 = BigInt(msg.return.ttbr1);
                        const kernelPgd = Number(ttbr1 & 0xFFFFFFFFF000n);
                        console.log(`\n✓✓✓ SUCCESS! Got kernel info from QMP:`);
                        console.log(`  TTBR1_EL1: 0x${ttbr1.toString(16)}`);
                        console.log(`  Kernel PGD: 0x${kernelPgd.toString(16)}\n`);
                        cleanup();
                        resolve({ success: true, kernelPgd });
                    } else if (msg.error) {
                        console.log('✗ QMP Error:', msg.error.desc);
                        responded = true;
                        cleanup();
                        resolve({ success: false, error: msg.error.desc });
                    }
                } catch (e) {
                    // Not JSON
                }
            }
        });

        socket.on('error', (err) => {
            console.error('✗ QMP connection error:', err.message);
            console.log('\nMake sure QEMU is running with -qmp tcp:localhost:4445,server,nowait');
            cleanup();
            resolve({ success: false, error: err.message });
        });

        // Timeout
        setTimeout(() => {
            if (!responded) {
                console.log('✗ QMP timeout - no response after 3 seconds');
                cleanup();
                resolve({ success: false, error: 'Timeout' });
            }
        }, 3000);
    });
}

testQMP().then(result => {
    if (result.success) {
        console.log('QMP test completed successfully!');
    } else {
        console.log('QMP test failed:', result.error);
    }
});