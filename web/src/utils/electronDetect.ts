/**
 * Detect if the application is running under Electron
 */
export function isElectron(): boolean {
  // Check for Electron-specific properties
  if (typeof window !== 'undefined') {
    // Check for Electron's process object
    if ((window as any).process?.type === 'renderer') {
      return true
    }

    // Check for Electron's navigator.userAgent
    if (navigator.userAgent.includes('Electron')) {
      return true
    }

    // Check for Electron-specific window properties
    // @ts-ignore
    if (window.electron || window.require) {
      return true
    }
  }

  return false
}

/**
 * Check if QMP features should be available
 */
export function isQMPAvailable(): boolean {
  return isElectron()
}

// Export a reactive ref that components can use
import { ref } from 'vue'
export const electronAvailable = ref(isElectron())
export const qmpAvailable = ref(isQMPAvailable())