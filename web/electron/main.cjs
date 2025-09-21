const { app, BrowserWindow, ipcMain, dialog } = require('electron')
const path = require('path')
const fs = require('fs')
// const { UniversalQGAClient } = require('../dist/utils/qgaClient') // TODO: Add back when QGA is ready

const DEFAULT_MEMORY_PATH = '/tmp/haywire-vm-mem'
let mainWindow
let qgaClient = null // TODO: Initialize when QGA is ready
let memoryFileFd = null
let currentFilePath = null

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.cjs')
    }
  })

  // In development, load from Vite dev server
  if (process.env.NODE_ENV === 'development') {
    // Ignore certificate errors for localhost development
    mainWindow.webContents.session.setCertificateVerifyProc((request, callback) => {
      // Allow self-signed certificates for localhost
      if (request.hostname === 'localhost') {
        callback(0) // 0 = OK
      } else {
        callback(-2) // -2 = Use default verification
      }
    })

    mainWindow.loadURL('https://localhost:3000')
    mainWindow.webContents.openDevTools()
  } else {
    // In production, load built files
    mainWindow.loadFile(path.join(__dirname, '../dist/index.html'))
  }
}

// IPC Handlers for QGA
function setupQGAHandlers() {
  // Connect to QGA
  ipcMain.handle('qga:connect', async (event, host = 'localhost', port = 4444) => {
    try {
      if (qgaClient) {
        qgaClient.disconnect()
      }

      qgaClient = new UniversalQGAClient(host, port)
      await qgaClient.connect()

      console.log('QGA connected successfully')
      return { success: true }
    } catch (error) {
      console.error('QGA connection failed:', error)
      return { success: false, error: error.message }
    }
  })

  // Disconnect from QGA
  ipcMain.handle('qga:disconnect', async () => {
    if (qgaClient) {
      qgaClient.disconnect()
      qgaClient = null
    }
    return { success: true }
  })

  // Ping QGA
  ipcMain.handle('qga:ping', async () => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      const result = await qgaClient.ping()
      return { success: true, alive: result }
    } catch (error) {
      return { success: false, error: error.message }
    }
  })

  // Get process list
  ipcMain.handle('qga:getProcesses', async () => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      const processes = await qgaClient.getProcessList()
      return { success: true, processes }
    } catch (error) {
      console.error('Failed to get processes:', error)
      return { success: false, error: error.message }
    }
  })

  // Get process details
  ipcMain.handle('qga:getProcessDetails', async (event, pid) => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      const details = await qgaClient.getProcessDetails(pid)
      return { success: true, details }
    } catch (error) {
      console.error(`Failed to get details for PID ${pid}:`, error)
      return { success: false, error: error.message }
    }
  })

  // Get memory maps
  ipcMain.handle('qga:getProcessMaps', async (event, pid) => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      const maps = await qgaClient.getProcessMaps(pid)
      return { success: true, maps }
    } catch (error) {
      return { success: false, error: error.message }
    }
  })

  // Execute command
  ipcMain.handle('qga:exec', async (event, command) => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      const output = await qgaClient.execShellCommand(command)
      return { success: true, output }
    } catch (error) {
      return { success: false, error: error.message }
    }
  })

  // Get memory info
  ipcMain.handle('qga:getMemoryInfo', async () => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      const info = await qgaClient.getMemoryInfo()
      return { success: true, info }
    } catch (error) {
      return { success: false, error: error.message }
    }
  })

  // Trigger beacon snapshot
  ipcMain.handle('qga:triggerSnapshot', async (event, pid = null) => {
    if (!qgaClient) {
      return { success: false, error: 'Not connected' }
    }

    try {
      // Create snapshot file with process info
      let script = `
#!/bin/sh
OUTPUT=/dev/shm/haywire_snapshot

# Write header with timestamp
echo "HAYWIRE_SNAPSHOT_V1" > $OUTPUT
date +%s >> $OUTPUT
echo "===PROCESSES===" >> $OUTPUT

# Get process list
for d in /proc/[0-9]*; do
  [ -d "$d" ] || continue
  p=\${d#/proc/}
  if [ -r "$d/comm" ]; then
    c=$(cat "$d/comm" 2>/dev/null | tr -d '\\n')
    echo "$p|$c" >> $OUTPUT
  fi
done
`

      // Add maps for specific PID if requested
      if (pid) {
        script += `
echo "===MAPS_${pid}===" >> $OUTPUT
cat /proc/${pid}/maps >> $OUTPUT 2>/dev/null
`
      }

      await qgaClient.execShellCommand(script)

      // Read back the snapshot
      const snapshot = await qgaClient.execShellCommand('cat /dev/shm/haywire_snapshot')

      return { success: true, snapshot }
    } catch (error) {
      return { success: false, error: error.message }
    }
  })
}

// File system access for memory dumps
ipcMain.handle('fs:selectMemoryFile', async () => {
  const { dialog } = require('electron')
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openFile'],
    filters: [
      { name: 'Memory Files', extensions: ['bin', 'raw', 'dump', 'mem'] },
      { name: 'All Files', extensions: ['*'] }
    ]
  })

  if (!result.canceled && result.filePaths.length > 0) {
    const fs = require('fs')
    const stats = fs.statSync(result.filePaths[0])

    return {
      path: result.filePaths[0],
      size: stats.size
    }
  }

  return null
})

// Memory File Handlers
ipcMain.handle('open-memory-file', async (event, filePath) => {
  try {
    const targetPath = filePath || DEFAULT_MEMORY_PATH

    // Close previous file if open
    if (memoryFileFd !== null) {
      fs.closeSync(memoryFileFd)
      memoryFileFd = null
    }

    // Check if file exists
    if (!fs.existsSync(targetPath)) {
      return {
        success: false,
        error: `File not found: ${targetPath}`,
        isDefaultPath: targetPath === DEFAULT_MEMORY_PATH
      }
    }

    memoryFileFd = fs.openSync(targetPath, 'r')
    const stats = fs.fstatSync(memoryFileFd)
    currentFilePath = targetPath

    return {
      success: true,
      size: stats.size,
      path: targetPath,
      isDefaultPath: targetPath === DEFAULT_MEMORY_PATH
    }
  } catch (error) {
    console.error('Error opening file:', error)
    return { success: false, error: error.message }
  }
})

ipcMain.handle('read-memory-chunk', async (event, offset, length) => {
  if (memoryFileFd === null) {
    return { success: false, error: 'No file open' }
  }

  try {
    const buffer = Buffer.alloc(length)
    const bytesRead = fs.readSync(memoryFileFd, buffer, 0, length, offset)

    return {
      success: true,
      data: new Uint8Array(buffer.slice(0, bytesRead)),
      bytesRead: bytesRead
    }
  } catch (error) {
    console.error('Error reading memory chunk:', error)
    return { success: false, error: error.message }
  }
})

ipcMain.handle('show-open-dialog', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Select Memory Backend File',
    defaultPath: '/tmp',
    filters: [
      { name: 'All Files', extensions: ['*'] },
      { name: 'Memory Files', extensions: ['mem', 'bin'] }
    ],
    properties: ['openFile']
  })

  if (!result.canceled && result.filePaths.length > 0) {
    return { success: true, path: result.filePaths[0] }
  }
  return { success: false, canceled: true }
})

ipcMain.handle('get-memory-status', async () => {
  return {
    isOpen: memoryFileFd !== null,
    path: currentFilePath,
    defaultPath: DEFAULT_MEMORY_PATH
  }
})

ipcMain.handle('close-memory-file', async () => {
  if (memoryFileFd !== null) {
    fs.closeSync(memoryFileFd)
    memoryFileFd = null
    currentFilePath = null
    return { success: true }
  }
  return { success: false, error: 'No file open' }
})

app.whenReady().then(() => {
  createWindow()
  // setupQGAHandlers() // TODO: Enable when QGA is ready
})

app.on('window-all-closed', () => {
  // Clean up resources
  if (qgaClient) {
    qgaClient.disconnect()
  }

  if (memoryFileFd !== null) {
    fs.closeSync(memoryFileFd)
    memoryFileFd = null
  }

  if (process.platform !== 'darwin') {
    app.quit()
  }
})

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow()
  }
})