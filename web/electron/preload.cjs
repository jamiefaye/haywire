const { contextBridge, ipcRenderer } = require('electron');

// Expose protected methods that allow the renderer process to use
// the ipcRenderer without exposing the entire object
contextBridge.exposeInMainWorld('electronAPI', {
  // Memory file operations
  openMemoryFile: (filePath) => ipcRenderer.invoke('open-memory-file', filePath),
  readMemoryChunk: (offset, length) => ipcRenderer.invoke('read-memory-chunk', offset, length),
  showOpenDialog: () => ipcRenderer.invoke('show-open-dialog'),
  getMemoryStatus: () => ipcRenderer.invoke('get-memory-status'),
  closeMemoryFile: () => ipcRenderer.invoke('close-memory-file'),

  // Convenience method for reading pages
  readMemoryPage: async (pageNumber) => {
    return ipcRenderer.invoke('read-memory-chunk', pageNumber * 4096, 4096);
  },

  // Check if running in Electron
  isElectron: true
});