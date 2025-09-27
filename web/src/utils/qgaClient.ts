/**
 * Universal QGA Client for Node.js/Electron
 * Communicates with QEMU Guest Agent via QMP
 */

import net from 'net'

interface QGAProcess {
  pid: number
  comm: string
  cmdline?: string
  maps?: string
}

interface QMPResponse {
  return?: any
  error?: {
    class: string
    desc: string
  }
  event?: string
  timestamp?: {
    seconds: number
    microseconds: number
  }
}

export class UniversalQGAClient {
  private socket: net.Socket | null = null
  private connected = false
  private responseHandlers = new Map<string, (response: QMPResponse) => void>()
  private eventHandlers = new Map<string, (event: any) => void>()
  private requestId = 0

  constructor(
    private host: string = 'localhost',
    private port: number = 4444
  ) {}

  /**
   * Connect to QMP socket
   */
  async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      this.socket = new net.Socket()

      this.socket.on('connect', () => {
        console.log('Connected to QMP socket')
      })

      this.socket.on('data', (data) => {
        // Handle multiple JSON responses in one chunk
        const lines = data.toString().split('\n').filter(line => line.trim())

        for (const line of lines) {
          try {
            const response = JSON.parse(line)
            this.handleResponse(response)
          } catch (e) {
            console.debug('Non-JSON QMP data:', line)
          }
        }
      })

      this.socket.on('error', (err) => {
        console.error('QMP socket error:', err)
        reject(err)
      })

      this.socket.on('close', () => {
        console.log('QMP socket closed')
        this.connected = false
      })

      // Connect and wait for banner
      this.socket.connect(this.port, this.host, async () => {
        // Wait for QMP banner
        await this.waitForResponse(100)

        // Send capabilities negotiation
        await this.execute('qmp_capabilities')

        this.connected = true
        resolve()
      })
    })
  }

  /**
   * Execute QMP command
   */
  private async execute(command: string, args?: any): Promise<any> {
    if (!this.socket || !this.connected) {
      throw new Error('Not connected to QMP')
    }

    const id = `req-${++this.requestId}`
    const request = {
      execute: command,
      arguments: args,
      id
    }

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.responseHandlers.delete(id)
        reject(new Error(`QMP command timeout: ${command}`))
      }, 30000)

      this.responseHandlers.set(id, (response) => {
        clearTimeout(timeout)
        this.responseHandlers.delete(id)

        if (response.error) {
          reject(new Error(`QMP error: ${response.error.desc}`))
        } else {
          resolve(response.return)
        }
      })

      this.socket!.write(JSON.stringify(request) + '\n')
    })
  }

  /**
   * Handle QMP response
   */
  private handleResponse(response: QMPResponse) {
    // Check if it's a response to a request
    const id = (response as any).id
    if (id && this.responseHandlers.has(id)) {
      this.responseHandlers.get(id)!(response)
      return
    }

    // Check if it's an event
    if (response.event) {
      const handler = this.eventHandlers.get(response.event)
      if (handler) {
        handler(response)
      }
    }
  }

  /**
   * Wait for any response (used during connection)
   */
  private waitForResponse(ms: number): Promise<void> {
    return new Promise(resolve => setTimeout(resolve, ms))
  }

  /**
   * Get list of processes using minimal POSIX shell
   */
  async getProcessList(): Promise<QGAProcess[]> {
    // Minimal POSIX shell script that works everywhere
    const script = `
      for d in /proc/[0-9]*; do
        [ -d "$d" ] || continue
        p=\${d#/proc/}
        if [ -r "$d/comm" ]; then
          c=$(cat "$d/comm" 2>/dev/null | tr -d '\\n')
          printf "%s|%s\\n" "$p" "$c"
        fi
      done
    `.trim()

    // Execute on guest
    const execResult = await this.execute('guest-exec', {
      'path': '/bin/sh',
      'arg': ['-c', script],
      'capture-output': true
    })

    const pid = execResult.pid

    // Poll for completion
    let attempts = 0
    while (attempts < 100) {
      const status = await this.execute('guest-exec-status', { pid })

      if (status.exited) {
        // Decode base64 output
        const output = Buffer.from(status['out-data'] || '', 'base64').toString('utf-8')

        // Parse into process list
        const processes: QGAProcess[] = []
        for (const line of output.split('\n')) {
          if (line.includes('|')) {
            const [pidStr, comm] = line.split('|', 2)
            processes.push({
              pid: parseInt(pidStr, 10),
              comm: comm.trim()
            })
          }
        }

        return processes
      }

      // Wait a bit before polling again
      await new Promise(resolve => setTimeout(resolve, 10))
      attempts++
    }

    throw new Error('Command execution timeout')
  }

  /**
   * Get memory maps for a specific process
   */
  async getProcessMaps(pid: number): Promise<string> {
    // Open /proc/PID/maps
    const handle = await this.execute('guest-file-open', {
      path: `/proc/${pid}/maps`,
      mode: 'r'
    })

    let maps = ''
    let chunk
    const maxReads = 100 // Safety limit

    try {
      for (let i = 0; i < maxReads; i++) {
        chunk = await this.execute('guest-file-read', {
          handle,
          count: 65536
        })

        if (chunk.count === 0 || chunk.eof) {
          break
        }

        // Decode base64 chunk
        maps += Buffer.from(chunk['buf-b64'], 'base64').toString('utf-8')
      }
    } finally {
      // Always close the file
      await this.execute('guest-file-close', { handle })
    }

    return maps
  }

  /**
   * Get detailed process information
   */
  async getProcessDetails(pid: number): Promise<QGAProcess> {
    const [comm, cmdline, maps] = await Promise.all([
      this.readProcFile(pid, 'comm'),
      this.readProcFile(pid, 'cmdline'),
      this.getProcessMaps(pid)
    ])

    return {
      pid,
      comm: comm.trim(),
      cmdline: cmdline.replace(/\0/g, ' ').trim(),
      maps
    }
  }

  /**
   * Read a file from /proc/PID/
   */
  private async readProcFile(pid: number, filename: string): Promise<string> {
    try {
      const handle = await this.execute('guest-file-open', {
        path: `/proc/${pid}/${filename}`,
        mode: 'r'
      })

      const chunk = await this.execute('guest-file-read', {
        handle,
        count: 4096
      })

      await this.execute('guest-file-close', { handle })

      return Buffer.from(chunk['buf-b64'] || '', 'base64').toString('utf-8')
    } catch (e) {
      return ''
    }
  }

  /**
   * Get system memory info
   */
  async getMemoryInfo(): Promise<any> {
    const script = `
      cat /proc/meminfo | head -10
    `.trim()

    const execResult = await this.execute('guest-exec', {
      'path': '/bin/sh',
      'arg': ['-c', script],
      'capture-output': true
    })

    const status = await this.waitForExecCompletion(execResult.pid)
    const output = Buffer.from(status['out-data'] || '', 'base64').toString('utf-8')

    // Parse meminfo
    const info: any = {}
    for (const line of output.split('\n')) {
      const match = line.match(/^(\w+):\s+(\d+)/)
      if (match) {
        info[match[1]] = parseInt(match[2], 10)
      }
    }

    return info
  }

  /**
   * Wait for command completion
   */
  private async waitForExecCompletion(pid: number, maxAttempts = 100): Promise<any> {
    for (let i = 0; i < maxAttempts; i++) {
      const status = await this.execute('guest-exec-status', { pid })
      if (status.exited) {
        return status
      }
      await new Promise(resolve => setTimeout(resolve, 10))
    }
    throw new Error('Command execution timeout')
  }

  /**
   * Execute arbitrary shell command
   */
  async execShellCommand(command: string): Promise<string> {
    const execResult = await this.execute('guest-exec', {
      'path': '/bin/sh',
      'arg': ['-c', command],
      'capture-output': true
    })

    const status = await this.waitForExecCompletion(execResult.pid)
    return Buffer.from(status['out-data'] || '', 'base64').toString('utf-8')
  }

  /**
   * Check if QGA is available
   */
  async ping(): Promise<boolean> {
    try {
      await this.execute('guest-ping')
      return true
    } catch {
      return false
    }
  }

  /**
   * Disconnect from QMP
   */
  disconnect() {
    if (this.socket) {
      this.socket.destroy()
      this.socket = null
    }
    this.connected = false
  }
}

// Export for use in Electron main process
export default UniversalQGAClient