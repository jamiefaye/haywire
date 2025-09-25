import * as net from 'net';

export interface KernelInfo {
    ttbr0?: bigint;
    ttbr1?: bigint;
    swapperPgDir?: number;
    kernelPgd?: number;
}

export class QMPClient {
    private host: string;
    private port: number;
    private socket: net.Socket | null = null;
    private connected: boolean = false;

    constructor(host: string = 'localhost', port: number = 4445) {
        this.host = host;
        this.port = port;
    }

    async connect(): Promise<void> {
        return new Promise((resolve, reject) => {
            this.socket = new net.Socket();
            let buffer = '';
            let capabilitiesSent = false;

            this.socket.connect(this.port, this.host, () => {
                console.log(`QMP: Connected to ${this.host}:${this.port}`);
            });

            this.socket.on('data', (data) => {
                buffer += data.toString();
                const lines = buffer.split('\n');
                buffer = lines.pop() || '';

                for (const line of lines) {
                    if (!line.trim()) continue;
                    
                    try {
                        const msg = JSON.parse(line);
                        
                        if (msg.QMP && !capabilitiesSent) {
                            // QMP banner received, send capabilities
                            this.socket!.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n');
                            capabilitiesSent = true;
                        } else if (capabilitiesSent && msg.return !== undefined) {
                            // Capabilities acknowledged
                            this.connected = true;
                            resolve();
                            return;
                        }
                    } catch (e) {
                        // Not JSON, ignore
                    }
                }
            });

            this.socket.on('error', (err) => {
                reject(err);
            });

            // Timeout after 3 seconds
            setTimeout(() => {
                if (!this.connected) {
                    this.socket?.destroy();
                    reject(new Error('QMP connection timeout'));
                }
            }, 3000);
        });
    }

    async queryKernelInfo(cpuIndex: number = 0): Promise<KernelInfo | null> {
        if (!this.connected || !this.socket) {
            console.log('QMP: Not connected');
            return null;
        }

        return new Promise((resolve) => {
            let responded = false;
            
            const handleResponse = (data: Buffer) => {
                const lines = data.toString().split('\n');
                
                for (const line of lines) {
                    if (!line.trim() || responded) continue;
                    
                    try {
                        const msg = JSON.parse(line);
                        
                        if (msg.return) {
                            responded = true;
                            
                            // Check if we got kernel info
                            if (msg.return.ttbr1 !== undefined) {
                                const info: KernelInfo = {
                                    ttbr0: BigInt(msg.return.ttbr0 || 0),
                                    ttbr1: BigInt(msg.return.ttbr1 || 0),
                                    swapperPgDir: msg.return.swapper_pg_dir,
                                    kernelPgd: msg.return.kernel_pgd
                                };
                                console.log(`QMP: Got kernel info - TTBR1=0x${info.ttbr1?.toString(16)}`);
                                resolve(info);
                            } else {
                                resolve(null);
                            }
                        } else if (msg.error) {
                            console.log(`QMP: Error - ${msg.error.desc}`);
                            responded = true;
                            resolve(null);
                        }
                    } catch (e) {
                        // Not JSON
                    }
                }
            };

            // Add one-time listener for response
            this.socket!.once('data', handleResponse);

            // Send query
            const query = {
                "execute": "query-kernel-info",
                "arguments": {"cpu-index": cpuIndex}
            };
            this.socket!.write(JSON.stringify(query) + '\n');

            // Timeout
            setTimeout(() => {
                if (!responded) {
                    console.log('QMP: Query timeout');
                    resolve(null);
                }
            }, 2000);
        });
    }

    disconnect(): void {
        if (this.socket) {
            this.socket.destroy();
            this.socket = null;
            this.connected = false;
        }
    }
}

// Helper function for one-shot queries
export async function queryKernelPGDViaQMP(): Promise<number | null> {
    // Check if running in Electron and use IPC
    if (typeof window !== 'undefined' && (window as any).electronAPI?.queryKernelInfo) {
        try {
            const result = await (window as any).electronAPI.queryKernelInfo();
            if (result.success && result.kernelPgd) {
                console.log(`QMP (via IPC): Kernel PGD from TTBR1_EL1 = 0x${result.kernelPgd.toString(16)}`);
                return result.kernelPgd;
            } else {
                console.log('QMP (via IPC): Failed -', result.error || 'No kernel PGD');
                return null;
            }
        } catch (err: any) {
            console.log('QMP (via IPC): Error -', err.message);
            return null;
        }
    }

    // Direct connection for non-Electron environments
    const client = new QMPClient();

    try {
        await client.connect();
        const info = await client.queryKernelInfo();
        client.disconnect();

        if (info?.ttbr1) {
            // TTBR1_EL1 contains the kernel PGD physical address
            // May need to mask off some bits depending on ARM64 config
            const pgd = Number(info.ttbr1 & 0xFFFFFFFFF000n);
            console.log(`QMP: Kernel PGD from TTBR1_EL1 = 0x${pgd.toString(16)}`);
            return pgd;
        }
    } catch (err: any) {
        console.log('QMP: Failed to query -', err.message);
    }

    return null;
}