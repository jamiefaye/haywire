// Type declarations for Electron API exposed via preload script

interface ElectronMemoryResult {
  success: boolean;
  error?: string;
}

interface ElectronFileOpenResult extends ElectronMemoryResult {
  size?: number;
  path?: string;
  isDefaultPath?: boolean;
}

interface ElectronMemoryChunkResult extends ElectronMemoryResult {
  data?: Uint8Array;
  bytesRead?: number;
}

interface ElectronDialogResult extends ElectronMemoryResult {
  path?: string;
  canceled?: boolean;
}

interface ElectronMemoryStatus {
  isOpen: boolean;
  path: string | null;
  defaultPath: string;
}

interface ElectronAPI {
  // Memory file operations
  openMemoryFile: (filePath?: string) => Promise<ElectronFileOpenResult>;
  readMemoryChunk: (offset: number, length: number) => Promise<ElectronMemoryChunkResult>;
  readMemoryPage: (pageNumber: number) => Promise<ElectronMemoryChunkResult>;
  showOpenDialog: () => Promise<ElectronDialogResult>;
  getMemoryStatus: () => Promise<ElectronMemoryStatus>;
  closeMemoryFile: () => Promise<ElectronMemoryResult>;

  // Flag to check if running in Electron
  isElectron: boolean;
}

declare global {
  interface Window {
    electronAPI?: ElectronAPI;
  }
}

export {};