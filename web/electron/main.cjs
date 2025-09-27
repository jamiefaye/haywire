const { app, BrowserWindow, ipcMain, dialog } = require('electron')
const path = require('path')
const fs = require('fs')
const net = require('net')
// const { UniversalQGAClient } = require('../dist/utils/qgaClient') // TODO: Add back when QGA is ready

// Disable certificate errors for development
if (process.env.NODE_ENV === 'development') {
  // Node.js environment variables
  process.env["NODE_TLS_REJECT_UNAUTHORIZED"] = "0";
  process.env["NODE_NO_WARNINGS"] = "1";

  // Electron command line switches
  app.commandLine.appendSwitch('ignore-certificate-errors')
  app.commandLine.appendSwitch('allow-insecure-localhost')
  app.commandLine.appendSwitch('disable-web-security')
}

const DEFAULT_MEMORY_PATH = '/tmp/haywire-vm-mem'
const LOG_FILE_PATH = '/tmp/haywire-kernel-discovery.log'
let mainWindow
let qgaClient = null // TODO: Initialize when QGA is ready
let memoryFileFd = null
let currentFilePath = null
let logFileStream = null

// Setup log file
function setupLogging() {
  // Create or truncate log file
  logFileStream = fs.createWriteStream(LOG_FILE_PATH, { flags: 'w' })
  logFileStream.write(`=== Haywire Kernel Discovery Log ===\n`)
  logFileStream.write(`Started: ${new Date().toISOString()}\n\n`)
  console.log('Logging to:', LOG_FILE_PATH)
}

function createWindow() {
  setupLogging()

  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.cjs')
    }
  })

  // Intercept console messages from the renderer
  mainWindow.webContents.on('console-message', (event, level, message, line, sourceId) => {
    const logLine = `[${new Date().toISOString()}] [${level}] ${message}\n`
    if (logFileStream) {
      logFileStream.write(logLine)
    }
  })

  // Check for diagnostic mode
  if (process.env.RUN_DIAGNOSTICS === 'true') {
    // Auto-run diagnostics after window loads
    mainWindow.webContents.on('did-finish-load', () => {
      console.log('AUTO-RUNNING KERNEL DIAGNOSTICS...');
      mainWindow.webContents.executeJavaScript(`
        (async () => {
          console.log('Starting automatic kernel diagnostics...');

          // Wait for file to be loaded (you need to set AUTO_FILE env var)
          const autoFile = '${process.env.AUTO_FILE || ''}';
          if (autoFile) {
            console.log('Auto-loading file:', autoFile);
            // Trigger file open
            // Note: This would need to be implemented in your file manager
          }

          // Wait a bit for everything to initialize
          setTimeout(async () => {
            if (window.runKernelDiagnostics) {
              console.log('Running diagnostics...');
              await window.runKernelDiagnostics();
            } else {
              console.log('Diagnostic function not found');
            }
          }, 3000);
        })();
      `);
    });
  }

  // In development, load from Vite dev server
  if (process.env.NODE_ENV === 'development') {
    // Disable certificate errors completely for development
    app.commandLine.appendSwitch('ignore-certificate-errors');

    // Suppress certificate error events
    mainWindow.webContents.on('certificate-error', (event, url, error, certificate, callback) => {
      event.preventDefault()
      callback(true) // Ignore the error and continue
    })

    // Also set certificate verify proc as backup
    mainWindow.webContents.session.setCertificateVerifyProc((request, callback) => {
      // Allow all certificates in development
      callback(0) // 0 = OK
    })

    // Set additional security preferences
    mainWindow.webContents.session.setPermissionRequestHandler((webContents, permission, callback) => {
      callback(true)
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
    // Safety check: don't allocate more than 100MB at once
    if (length > 100 * 1024 * 1024) {
      console.error(`Requested chunk too large: ${length} bytes`)
      return { success: false, error: `Chunk size too large: ${length} bytes (max 100MB)` }
    }

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

// IPC handler to read the log file
ipcMain.handle('read-log-file', async () => {
  try {
    if (!fs.existsSync(LOG_FILE_PATH)) {
      return { success: false, error: 'Log file not found' }
    }
    const content = fs.readFileSync(LOG_FILE_PATH, 'utf8')
    return { success: true, content }
  } catch (error) {
    return { success: false, error: error.message }
  }
})

// IPC handler to clear the log file
ipcMain.handle('clear-log-file', async () => {
  try {
    if (logFileStream) {
      logFileStream.end()
    }
    setupLogging()
    return { success: true }
  } catch (error) {
    return { success: false, error: error.message }
  }
})

// IPC handler to trigger kernel discovery
ipcMain.handle('trigger-kernel-discovery', async () => {
  try {
    // Clear the log first
    if (logFileStream) {
      logFileStream.write('\n=== Kernel Discovery Triggered ===\n')
      logFileStream.write(`Time: ${new Date().toISOString()}\n\n`)
    }

    // Tell the renderer to run kernel discovery
    mainWindow.webContents.executeJavaScript(`
      (async () => {
        console.log('Starting kernel discovery from IPC trigger...');
        if (window.runKernelDiscovery) {
          const result = await window.runKernelDiscovery();
          console.log('Kernel discovery completed');
          return result;
        } else {
          console.error('runKernelDiscovery function not found');
          return null;
        }
      })()
    `).then(result => {
      if (logFileStream) {
        logFileStream.write('\n=== Discovery Result ===\n')
        logFileStream.write(JSON.stringify(result, null, 2) + '\n')
      }
    })

    return { success: true, logPath: LOG_FILE_PATH }
  } catch (error) {
    return { success: false, error: error.message }
  }
})

// QMP Handler for kernel info
ipcMain.handle('qmp:queryKernelInfo', async () => {
  return new Promise((resolve) => {
    const socket = new net.Socket()
    let buffer = ''
    let capabilitiesSent = false
    let responded = false

    const cleanup = () => {
      if (!socket.destroyed) {
        socket.destroy()
      }
    }

    socket.connect(4445, 'localhost', () => {
      console.log('QMP: Connected to localhost:4445')
    })

    socket.on('data', (data) => {
      buffer += data.toString()
      const lines = buffer.split('\n')
      buffer = lines.pop() || ''

      for (const line of lines) {
        if (!line.trim() || responded) continue

        try {
          const msg = JSON.parse(line)

          if (msg.QMP && !capabilitiesSent) {
            socket.write(JSON.stringify({"execute": "qmp_capabilities"}) + '\n')
            capabilitiesSent = true
          } else if (capabilitiesSent && msg.return !== undefined && !msg.return.ttbr1) {
            // Send query-kernel-info
            socket.write(JSON.stringify({
              "execute": "query-kernel-info",
              "arguments": {"cpu-index": 0}
            }) + '\n')
          } else if (msg.return && msg.return.ttbr1) {
            responded = true
            const kernelPgd = Number(BigInt(msg.return.ttbr1) & 0xFFFFFFFFF000n)
            console.log(`QMP: Kernel PGD from TTBR1_EL1 = 0x${kernelPgd.toString(16)}`)
            cleanup()
            resolve({ success: true, kernelPgd })
          } else if (msg.error) {
            console.log('QMP Error:', msg.error.desc)
            responded = true
            cleanup()
            resolve({ success: false, error: msg.error.desc })
          }
        } catch (e) {
          // Not JSON
        }
      }
    })

    socket.on('error', (err) => {
      console.error('QMP connection error:', err.message)
      cleanup()
      resolve({ success: false, error: err.message })
    })

    // Timeout
    setTimeout(() => {
      if (!responded) {
        console.log('QMP: Query timeout')
        cleanup()
        resolve({ success: false, error: 'Timeout' })
      }
    }, 3000)
  })
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

  if (logFileStream) {
    logFileStream.end()
    logFileStream = null
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