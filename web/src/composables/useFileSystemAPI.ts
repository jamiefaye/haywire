// Vue composable for File System API to read memory-mapped file
import { ref, shallowRef } from 'vue'

export interface MemoryFileHandle {
  handle: FileSystemFileHandle
  file: File
  lastModified: number
}

export function useFileSystemAPI() {
  const fileHandle = shallowRef<FileSystemFileHandle | null>(null)
  const isFileOpen = ref(false)
  const fileName = ref<string>('')
  const fileSize = ref<number>(0)
  const error = ref<string | null>(null)

  // Open file picker for memory-mapped file
  async function openMemoryFile(): Promise<boolean> {
    try {
      // Show file picker - accept any file type
      const [handle] = await window.showOpenFilePicker({
        multiple: false
        // No type restrictions - can open any file
      })

      fileHandle.value = handle
      fileName.value = handle.name

      // Get initial file info
      const file = await handle.getFile()
      fileSize.value = file.size
      isFileOpen.value = true
      error.value = null

      console.log(`Opened memory file: ${handle.name} (${file.size} bytes)`)
      return true
    } catch (err) {
      if ((err as Error).name !== 'AbortError') {
        error.value = `Failed to open file: ${err}`
        console.error('Error opening file:', err)
      }
      return false
    }
  }

  // Open a specific file path (if user grants permission)
  async function openSpecificFile(path: string): Promise<boolean> {
    try {
      // For security, we can't directly open a path
      // User must select it, but we can suggest the directory
      const suggestedName = path.split('/').pop() || 'haywire-vm-mem'

      const [handle] = await window.showOpenFilePicker({
        startIn: 'desktop',
        suggestedName,
        multiple: false
      })

      fileHandle.value = handle
      fileName.value = handle.name

      const file = await handle.getFile()
      fileSize.value = file.size
      isFileOpen.value = true
      error.value = null

      return true
    } catch (err) {
      if ((err as Error).name !== 'AbortError') {
        error.value = `Failed to open file: ${err}`
      }
      return false
    }
  }

  // Read a chunk of the file
  async function readMemoryChunk(offset: number, size: number): Promise<Uint8Array | null> {
    if (!fileHandle.value) {
      error.value = 'No file is open'
      return null
    }

    try {
      // Get fresh file to ensure we have latest content
      const file = await fileHandle.value.getFile()

      // Clamp size to file bounds
      const actualSize = Math.min(size, file.size - offset)
      if (actualSize <= 0) {
        error.value = `Invalid read range: offset ${offset}, size ${size}, file size ${file.size}`
        return null
      }

      // Read the slice
      const slice = file.slice(offset, offset + actualSize)
      const buffer = await slice.arrayBuffer()

      return new Uint8Array(buffer)
    } catch (err) {
      error.value = `Failed to read file: ${err}`
      console.error('Error reading file:', err)
      return null
    }
  }

  // Read entire file (use with caution for large files!)
  async function readEntireFile(): Promise<Uint8Array | null> {
    if (!fileHandle.value) {
      error.value = 'No file is open'
      return null
    }

    try {
      const file = await fileHandle.value.getFile()
      const buffer = await file.arrayBuffer()
      return new Uint8Array(buffer)
    } catch (err) {
      error.value = `Failed to read entire file: ${err}`
      return null
    }
  }

  // Get current file stats
  async function getFileStats() {
    if (!fileHandle.value) {
      return null
    }

    try {
      const file = await fileHandle.value.getFile()
      return {
        name: file.name,
        size: file.size,
        lastModified: file.lastModified,
        type: file.type
      }
    } catch (err) {
      error.value = `Failed to get file stats: ${err}`
      return null
    }
  }

  // Create a readable stream for the file
  async function createReadableStream(): Promise<ReadableStream<Uint8Array> | null> {
    if (!fileHandle.value) {
      error.value = 'No file is open'
      return null
    }

    try {
      const file = await fileHandle.value.getFile()
      return file.stream() as ReadableStream<Uint8Array>
    } catch (err) {
      error.value = `Failed to create stream: ${err}`
      return null
    }
  }

  // Watch for file changes (polling-based)
  function watchFile(callback: (stats: any) => void, interval: number = 1000) {
    if (!fileHandle.value) {
      error.value = 'No file is open'
      return null
    }

    let lastModified = 0
    let lastSize = 0

    const checkInterval = setInterval(async () => {
      if (!fileHandle.value) {
        clearInterval(checkInterval)
        return
      }

      try {
        const file = await fileHandle.value.getFile()
        if (file.lastModified !== lastModified || file.size !== lastSize) {
          lastModified = file.lastModified
          lastSize = file.size
          fileSize.value = file.size

          callback({
            name: file.name,
            size: file.size,
            lastModified: file.lastModified
          })
        }
      } catch (err) {
        console.error('Error checking file changes:', err)
      }
    }, interval)

    // Return cleanup function
    return () => clearInterval(checkInterval)
  }

  // Close the file handle
  function closeFile() {
    fileHandle.value = null
    isFileOpen.value = false
    fileName.value = ''
    fileSize.value = 0
    error.value = null
  }

  return {
    // State
    fileHandle,
    isFileOpen,
    fileName,
    fileSize,
    error,

    // Methods
    openMemoryFile,
    openSpecificFile,
    readMemoryChunk,
    readEntireFile,
    getFileStats,
    createReadableStream,
    watchFile,
    closeFile
  }
}