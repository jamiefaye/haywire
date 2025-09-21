<template>
  <div class="memory-file-manager">
    <div class="file-status">
      <span class="status-icon" :class="{ connected: isConnected }">●</span>
      <span class="file-path" :title="currentPath">
        {{ displayPath }}
      </span>
      <span v-if="fileSize > 0" class="file-size">
        ({{ formatSize(fileSize) }})
      </span>
    </div>

    <div class="controls">
      <button
        @click="openDefault"
        :disabled="isConnected && isUsingDefault"
        title="Open default Haywire VM memory"
        class="btn-default"
      >
        {{ isConnected && isUsingDefault ? '✓ Default' : 'Use Default' }}
      </button>

      <button
        @click="browseFile"
        title="Browse for custom memory file"
        class="btn-browse"
      >
        Browse...
      </button>

      <button
        v-if="isConnected && !isUsingDefault"
        @click="openDefault"
        class="btn-reset"
        title="Switch back to default path"
      >
        Reset to Default
      </button>

      <button
        v-if="isConnected"
        @click="toggleAutoRefresh"
        :title="autoRefresh ? 'Disable auto-refresh' : 'Enable auto-refresh'"
        :class="['btn-auto-refresh', { active: autoRefresh }]"
      >
        {{ autoRefresh ? '⏸ Auto' : '▶ Auto' }}
      </button>

      <button
        v-if="isConnected"
        @click="refresh"
        title="Reload file"
        class="btn-refresh"
      >
        🔄 Refresh
      </button>

      <button
        v-if="isConnected"
        @click="closeFile"
        title="Close file"
        class="btn-close"
      >
        ✕ Close
      </button>
    </div>

    <div v-if="error" class="error">
      <span>{{ error }}</span>
      <button v-if="errorIsDefaultMissing" @click="browseFile" class="btn-inline">
        Select File
      </button>
    </div>
  </div>
</template>

<script>
export default {
  name: 'MemoryFileManager',

  data() {
    return {
      isConnected: false,
      currentPath: null,
      defaultPath: '/tmp/haywire-vm-mem',
      fileSize: 0,
      error: null,
      errorIsDefaultMissing: false,
      isElectron: false,
      autoRefresh: true,
      refreshInterval: null,
      refreshRate: 250, // milliseconds
      lastRefreshTime: null,
      refreshCount: 0
    };
  },

  computed: {
    isUsingDefault() {
      return this.currentPath === this.defaultPath;
    },

    displayPath() {
      if (!this.currentPath) return 'No file open';
      if (this.isUsingDefault) return 'Haywire VM Memory (default)';

      // Show just filename for custom paths
      const parts = this.currentPath.split('/');
      return parts[parts.length - 1] || this.currentPath;
    }
  },

  async mounted() {
    // Check if running in Electron
    this.isElectron = window.electronAPI && window.electronAPI.isElectron;

    if (!this.isElectron) {
      this.error = 'Not running in Electron - file access limited';
      return;
    }

    // Try to open default file on startup
    await this.openDefault();

    // Check status
    const status = await window.electronAPI.getMemoryStatus();
    if (status.defaultPath) {
      this.defaultPath = status.defaultPath;
    }
  },

  beforeUnmount() {
    // Clean up refresh interval
    this.stopAutoRefresh();
  },

  methods: {
    async openDefault() {
      if (!this.isElectron) return;

      this.error = null;

      try {
        const result = await window.electronAPI.openMemoryFile();

        if (result.success) {
          this.isConnected = true;
          this.currentPath = result.path;
          this.fileSize = result.size;
          this.$emit('file-opened', {
            path: result.path,
            size: result.size
          });

          // Start auto-refresh if enabled
          if (this.autoRefresh) {
            this.startAutoRefresh();
          }
        } else {
          this.error = result.error;
          this.errorIsDefaultMissing = result.error.includes('not found') &&
                                       result.isDefaultPath;

          if (this.errorIsDefaultMissing) {
            this.error = 'Default VM memory not found. Is QEMU running?';
          }
        }
      } catch (err) {
        this.error = `Failed to open file: ${err.message}`;
      }
    },

    async browseFile() {
      if (!this.isElectron) return;

      try {
        const dialogResult = await window.electronAPI.showOpenDialog();

        if (dialogResult.success) {
          const openResult = await window.electronAPI.openMemoryFile(dialogResult.path);

          if (openResult.success) {
            this.isConnected = true;
            this.currentPath = openResult.path;
            this.fileSize = openResult.size;
            this.error = null;

            this.$emit('file-opened', {
              path: openResult.path,
              size: openResult.size
            });

            // Start auto-refresh if enabled
            if (this.autoRefresh) {
              this.startAutoRefresh();
            }
          } else {
            this.error = `Failed to open: ${openResult.error}`;
          }
        }
      } catch (err) {
        this.error = `Dialog error: ${err.message}`;
      }
    },

    async refresh() {
      if (!this.isElectron || !this.currentPath || !this.isConnected) return;

      // Measure time since last refresh
      const now = Date.now();
      if (this.lastRefreshTime) {
        const delta = now - this.lastRefreshTime;
        this.refreshCount++;
        console.log(`Data refresh #${this.refreshCount}: ${delta}ms since last refresh`);
      }
      this.lastRefreshTime = now;

      // Don't reopen the file - just emit refresh event
      // The file is already open in the main process
      this.$emit('file-refreshed', {
        path: this.currentPath,
        size: this.fileSize
      });
    },

    async closeFile() {
      if (!this.isElectron) return;

      try {
        // Stop auto-refresh
        this.stopAutoRefresh();

        await window.electronAPI.closeMemoryFile();
        this.isConnected = false;
        this.currentPath = null;
        this.fileSize = 0;
        this.error = null;
        this.$emit('file-closed');
      } catch (err) {
        this.error = `Close error: ${err.message}`;
      }
    },

    startAutoRefresh() {
      // Clear any existing interval
      this.stopAutoRefresh();

      // Set up new interval
      this.refreshInterval = setInterval(() => {
        this.refresh();
      }, this.refreshRate);
    },

    stopAutoRefresh() {
      if (this.refreshInterval) {
        clearInterval(this.refreshInterval);
        this.refreshInterval = null;
      }
    },

    toggleAutoRefresh() {
      this.autoRefresh = !this.autoRefresh;

      if (this.autoRefresh && this.isConnected) {
        this.startAutoRefresh();
      } else {
        this.stopAutoRefresh();
      }
    },

    formatSize(bytes) {
      const mb = bytes / (1024 * 1024);
      if (mb >= 1024) {
        return `${(mb / 1024).toFixed(2)} GB`;
      }
      return `${mb.toFixed(2)} MB`;
    }
  }
};
</script>

<style scoped>
.memory-file-manager {
  padding: 12px;
  background: #1e1e1e;
  border-bottom: 1px solid #3a3a3a;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
}

.file-status {
  display: flex;
  align-items: center;
  margin-bottom: 10px;
  font-size: 14px;
}

.status-icon {
  margin-right: 8px;
  font-size: 12px;
  color: #666;
}

.status-icon.connected {
  color: #4CAF50;
}

.file-path {
  font-family: 'SF Mono', Monaco, 'Courier New', monospace;
  color: #b0b0b0;
}

.file-size {
  margin-left: 8px;
  color: #888;
  font-size: 13px;
}

.controls {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}

.controls button {
  padding: 6px 12px;
  background: #2d2d2d;
  border: 1px solid #3a3a3a;
  color: #e0e0e0;
  border-radius: 4px;
  font-size: 13px;
  cursor: pointer;
  transition: all 0.2s;
}

.controls button:hover:not(:disabled) {
  background: #3a3a3a;
  border-color: #4a4a4a;
}

.controls button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.btn-default:disabled {
  background: #1a4a2a;
  border-color: #2a5a3a;
}

.btn-reset {
  background: #3a3a2d;
}

.btn-close {
  background: #4a2d2d;
}

.btn-refresh {
  font-size: 16px;
  padding: 4px 10px;
}

.btn-auto-refresh {
  background: #2d3d4d;
  border-color: #3d4d5d;
}

.btn-auto-refresh.active {
  background: #2d4d3d;
  border-color: #3d5d4d;
  color: #8fc98f;
}

.error {
  margin-top: 10px;
  padding: 10px;
  background: #3a2020;
  color: #ff6b6b;
  border: 1px solid #4a2a2a;
  border-radius: 4px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 13px;
}

.btn-inline {
  padding: 4px 10px;
  background: #2d2d2d;
  border: 1px solid #3a3a3a;
  color: #e0e0e0;
  border-radius: 3px;
  font-size: 12px;
  cursor: pointer;
}

.btn-inline:hover {
  background: #3a3a3a;
}
</style>