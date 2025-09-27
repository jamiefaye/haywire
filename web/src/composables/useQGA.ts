/**
 * Vue composable for QGA communication via Electron IPC
 */

import { ref, onUnmounted } from 'vue'
import { isElectron } from '../utils/electronDetect'

interface QGAProcess {
  pid: number
  comm: string
  cmdline?: string
  maps?: string
}

export function useQGA() {
  const isConnected = ref(false)
  const isConnecting = ref(false)
  const processes = ref<QGAProcess[]>([])
  const error = ref<string | null>(null)

  // Get the Electron IPC renderer
  const ipcRenderer = isElectron() ? (window as any).electron?.ipcRenderer : null

  /**
   * Connect to QGA via Electron main process
   */
  async function connect() {
    if (!ipcRenderer) {
      error.value = 'QGA requires Electron environment'
      return false
    }

    if (isConnected.value || isConnecting.value) {
      return isConnected.value
    }

    try {
      isConnecting.value = true
      error.value = null

      await ipcRenderer.invoke('qga:connect')
      isConnected.value = true
      return true
    } catch (err) {
      error.value = `Failed to connect: ${err}`
      isConnected.value = false
      return false
    } finally {
      isConnecting.value = false
    }
  }

  /**
   * Get list of processes
   */
  async function getProcessList(): Promise<QGAProcess[]> {
    if (!isConnected.value) {
      const connected = await connect()
      if (!connected) return []
    }

    try {
      const result = await ipcRenderer.invoke('qga:getProcesses')
      processes.value = result
      return result
    } catch (err) {
      error.value = `Failed to get processes: ${err}`
      return []
    }
  }

  /**
   * Get detailed process information
   */
  async function getProcessDetails(pid: number): Promise<QGAProcess | null> {
    if (!isConnected.value) {
      const connected = await connect()
      if (!connected) return null
    }

    try {
      return await ipcRenderer.invoke('qga:getProcessDetails', pid)
    } catch (err) {
      error.value = `Failed to get process details: ${err}`
      return null
    }
  }

  /**
   * Execute shell command on guest
   */
  async function execCommand(command: string): Promise<string> {
    if (!isConnected.value) {
      const connected = await connect()
      if (!connected) return ''
    }

    try {
      return await ipcRenderer.invoke('qga:exec', command)
    } catch (err) {
      error.value = `Failed to execute command: ${err}`
      return ''
    }
  }

  /**
   * Get memory maps for a process
   */
  async function getProcessMaps(pid: number): Promise<string> {
    const details = await getProcessDetails(pid)
    return details?.maps || ''
  }

  /**
   * Refresh process list periodically
   */
  let refreshInterval: number | null = null
  let mapsRefreshInterval: number | null = null
  const selectedPid = ref<number | null>(null)

  function startAutoRefresh(intervalMs: number = 5000) { // Default 5 seconds for PID list
    stopAutoRefresh()
    refreshInterval = window.setInterval(() => {
      getProcessList()
    }, intervalMs)
  }

  /**
   * Auto-refresh memory maps for selected process
   */
  function startMapsAutoRefresh(pid: number, intervalMs: number = 2000) { // Default 2 seconds for maps
    stopMapsAutoRefresh()
    selectedPid.value = pid

    // Get initial maps
    getProcessDetails(pid)

    mapsRefreshInterval = window.setInterval(() => {
      if (selectedPid.value === pid) {
        getProcessDetails(pid)
      }
    }, intervalMs)
  }

  function stopMapsAutoRefresh() {
    if (mapsRefreshInterval !== null) {
      clearInterval(mapsRefreshInterval)
      mapsRefreshInterval = null
    }
  }

  function stopAutoRefresh() {
    if (refreshInterval !== null) {
      clearInterval(refreshInterval)
      refreshInterval = null
    }
  }

  /**
   * Disconnect on unmount
   */
  async function disconnect() {
    stopAutoRefresh()
    stopMapsAutoRefresh()
    if (ipcRenderer && isConnected.value) {
      try {
        await ipcRenderer.invoke('qga:disconnect')
        isConnected.value = false
      } catch (err) {
        console.error('Error disconnecting QGA:', err)
      }
    }
  }

  onUnmounted(() => {
    disconnect()
  })

  return {
    // State
    isConnected,
    isConnecting,
    processes,
    error,
    selectedPid,

    // Methods
    connect,
    disconnect,
    getProcessList,
    getProcessDetails,
    getProcessMaps,
    execCommand,
    startAutoRefresh,
    stopAutoRefresh,
    startMapsAutoRefresh,
    stopMapsAutoRefresh
  }
}