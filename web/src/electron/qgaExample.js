/**
 * Example usage of QGA client in Electron main process
 */

const { UniversalQGAClient } = require('../utils/qgaClient')

async function demonstrateQGA() {
  const qga = new UniversalQGAClient('localhost', 4444)

  try {
    // Connect to QMP socket
    console.log('Connecting to QMP...')
    await qga.connect()

    // Check if guest agent is responding
    console.log('Pinging guest agent...')
    const isAlive = await qga.ping()
    console.log('Guest agent available:', isAlive)

    // Get process list
    console.log('\nGetting process list...')
    const processes = await qga.getProcessList()
    console.log(`Found ${processes.length} processes`)

    // Show first 10 processes
    processes.slice(0, 10).forEach(p => {
      console.log(`  PID ${p.pid}: ${p.comm}`)
    })

    // Get detailed info for init process
    console.log('\nGetting details for PID 1...')
    const initProcess = await qga.getProcessDetails(1)
    console.log('Init process:', initProcess.comm)
    console.log('Command line:', initProcess.cmdline)
    console.log('Maps preview:', initProcess.maps?.substring(0, 200))

    // Get memory info
    console.log('\nGetting memory info...')
    const memInfo = await qga.getMemoryInfo()
    console.log('Total memory:', memInfo.MemTotal, 'KB')
    console.log('Free memory:', memInfo.MemFree, 'KB')

    // Execute custom command
    console.log('\nExecuting custom command...')
    const uptime = await qga.execShellCommand('uptime')
    console.log('Uptime:', uptime.trim())

  } catch (error) {
    console.error('Error:', error)
  } finally {
    qga.disconnect()
  }
}

// For use in Electron main process
function setupQGAHandlers(ipcMain) {
  let qgaClient = null

  // Connect to QGA
  ipcMain.handle('qga:connect', async () => {
    if (!qgaClient) {
      qgaClient = new UniversalQGAClient()
      await qgaClient.connect()
    }
    return true
  })

  // Get process list
  ipcMain.handle('qga:getProcesses', async () => {
    if (!qgaClient) throw new Error('QGA not connected')
    return await qgaClient.getProcessList()
  })

  // Get process details
  ipcMain.handle('qga:getProcessDetails', async (event, pid) => {
    if (!qgaClient) throw new Error('QGA not connected')
    return await qgaClient.getProcessDetails(pid)
  })

  // Execute shell command
  ipcMain.handle('qga:exec', async (event, command) => {
    if (!qgaClient) throw new Error('QGA not connected')
    return await qgaClient.execShellCommand(command)
  })

  // Disconnect
  ipcMain.handle('qga:disconnect', () => {
    if (qgaClient) {
      qgaClient.disconnect()
      qgaClient = null
    }
    return true
  })
}

module.exports = {
  demonstrateQGA,
  setupQGAHandlers
}