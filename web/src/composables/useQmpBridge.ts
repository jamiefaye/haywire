// Vue composable for WebSocket connection to QMP bridge
import { ref, onUnmounted } from 'vue'
import { isElectron } from '../utils/electronDetect'

interface QmpRequest {
  execute: string
  arguments?: any
  id?: string
}

interface QmpResponse {
  return?: any
  error?: {
    class: string
    desc: string
  }
  id?: string
}

export function useQmpBridge(bridgeUrl: string = 'ws://localhost:8080') {
  const ws = ref<WebSocket | null>(null)
  const isConnected = ref(false)
  const isConnecting = ref(false)
  const error = ref<string | null>(null)

  // Pending requests waiting for responses
  const pendingRequests = new Map<string, {
    resolve: (value: any) => void
    reject: (error: any) => void
  }>()

  let requestId = 0

  // Connect to the QMP bridge
  async function connect(): Promise<boolean> {
    // Only allow connection in Electron environment
    if (!isElectron()) {
      error.value = 'QMP connection requires Electron environment'
      console.warn('QMP connection attempted outside Electron environment')
      return false
    }

    if (isConnected.value || isConnecting.value) {
      return isConnected.value
    }

    isConnecting.value = true
    error.value = null

    return new Promise((resolve) => {
      try {
        ws.value = new WebSocket(bridgeUrl)

        ws.value.onopen = () => {
          console.log('Connected to QMP bridge')
          isConnected.value = true
          isConnecting.value = false
          error.value = null

          // Send QMP capabilities negotiation
          sendCommand('qmp_capabilities').catch(console.error)

          resolve(true)
        }

        ws.value.onmessage = (event) => {
          try {
            const response: QmpResponse = JSON.parse(event.data)

            // Handle response with ID
            if (response.id && pendingRequests.has(response.id)) {
              const pending = pendingRequests.get(response.id)!
              pendingRequests.delete(response.id)

              if (response.error) {
                pending.reject(new Error(response.error.desc))
              } else {
                pending.resolve(response.return)
              }
            }

            // Handle events (no ID)
            if (!response.id && !response.error && !response.return) {
              console.log('QMP Event:', response)
            }
          } catch (err) {
            console.error('Failed to parse QMP response:', err)
          }
        }

        ws.value.onerror = (err) => {
          error.value = `WebSocket error: ${err}`
          console.error('WebSocket error:', err)
        }

        ws.value.onclose = () => {
          isConnected.value = false
          isConnecting.value = false

          // Reject all pending requests
          for (const [id, pending] of pendingRequests) {
            pending.reject(new Error('Connection closed'))
          }
          pendingRequests.clear()

          console.log('Disconnected from QMP bridge')
          resolve(false)
        }
      } catch (err) {
        error.value = `Failed to connect: ${err}`
        isConnecting.value = false
        resolve(false)
      }
    })
  }

  // Send a QMP command
  async function sendCommand(command: string, args?: any): Promise<any> {
    if (!isConnected.value || !ws.value) {
      throw new Error('Not connected to QMP bridge')
    }

    const id = `req_${++requestId}`
    const request: QmpRequest = {
      execute: command,
      arguments: args,
      id
    }

    return new Promise((resolve, reject) => {
      pendingRequests.set(id, { resolve, reject })

      ws.value!.send(JSON.stringify(request))

      // Timeout after 30 seconds
      setTimeout(() => {
        if (pendingRequests.has(id)) {
          pendingRequests.delete(id)
          reject(new Error(`Command timeout: ${command}`))
        }
      }, 30000)
    })
  }

  // Translate virtual address to physical address
  async function translateVirtualAddress(
    va: number,
    cpuIndex: number = 0
  ): Promise<{ pa: number, valid: boolean }> {
    try {
      const result = await sendCommand('query-va2pa', {
        'cpu-index': cpuIndex,
        'virtual-address': va
      })

      return {
        pa: result['physical-address'],
        valid: result.valid !== false
      }
    } catch (err) {
      console.error('Failed to translate VA:', err)
      return { pa: 0, valid: false }
    }
  }

  // Get kernel information
  async function getKernelInfo(cpuIndex: number = 0): Promise<any> {
    try {
      return await sendCommand('query-kernel-info', {
        'cpu-index': cpuIndex
      })
    } catch (err) {
      console.error('Failed to get kernel info:', err)
      return null
    }
  }

  // Read physical memory
  async function readPhysicalMemory(
    address: number,
    size: number,
    cpuIndex: number = 0
  ): Promise<Uint8Array | null> {
    try {
      const result = await sendCommand('query-physical-memory', {
        'cpu-index': cpuIndex,
        'address': address,
        'size': size
      })

      // Result should be base64 encoded
      const base64 = result.data
      const binaryString = atob(base64)
      const bytes = new Uint8Array(binaryString.length)
      for (let i = 0; i < binaryString.length; i++) {
        bytes[i] = binaryString.charCodeAt(i)
      }

      return bytes
    } catch (err) {
      console.error('Failed to read physical memory:', err)
      return null
    }
  }

  // Get CPU registers
  async function getCpuRegisters(cpuIndex: number = 0): Promise<any> {
    try {
      return await sendCommand('query-cpu-registers', {
        'cpu-index': cpuIndex
      })
    } catch (err) {
      console.error('Failed to get CPU registers:', err)
      return null
    }
  }

  // Disconnect from bridge
  function disconnect() {
    if (ws.value) {
      ws.value.close()
      ws.value = null
    }
    isConnected.value = false
    pendingRequests.clear()
  }

  // Cleanup on unmount
  onUnmounted(() => {
    disconnect()
  })

  return {
    // State
    isConnected,
    isConnecting,
    error,

    // Connection
    connect,
    disconnect,

    // Commands
    sendCommand,
    translateVirtualAddress,
    getKernelInfo,
    readPhysicalMemory,
    getCpuRegisters
  }
}