<template>
  <div class="process-explorer">
    <div class="header">
      <h3>Process Explorer (QGA)</h3>
      <div class="controls">
        <button v-if="!isConnected" @click="connect" :disabled="isConnecting">
          {{ isConnecting ? 'Connecting...' : 'Connect to Guest' }}
        </button>
        <button v-else @click="disconnect">
          Disconnect
        </button>
        <button @click="refresh" :disabled="!isConnected || isLoading">
          🔄 Refresh
        </button>
        <label title="Refresh process list every 5 seconds">
          <input
            type="checkbox"
            v-model="autoRefresh"
            @change="toggleAutoRefresh"
            :disabled="!isConnected"
          >
          Auto-refresh (5s)
        </label>
      </div>
    </div>

    <div v-if="error" class="error">
      {{ error }}
    </div>

    <div v-if="isConnected" class="content">
      <div class="process-list">
        <table>
          <thead>
            <tr>
              <th>PID</th>
              <th>Name</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            <tr
              v-for="process in filteredProcesses"
              :key="process.pid"
              :class="{ selected: selectedPid === process.pid }"
              @click="selectProcess(process.pid)"
            >
              <td>{{ process.pid }}</td>
              <td>{{ process.comm }}</td>
              <td>
                <button @click.stop="showMaps(process.pid)" size="small">
                  Maps
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <div v-if="selectedProcess" class="process-details">
        <h4>
          Process Details: {{ selectedProcess.comm }} (PID {{ selectedProcess.pid }})
          <span class="refresh-indicator" title="Maps refresh every 2 seconds">
            ↻ 2s
          </span>
        </h4>
        <div class="detail-row">
          <strong>Command:</strong> {{ selectedProcess.comm }}
        </div>
        <div v-if="selectedProcess.cmdline" class="detail-row">
          <strong>Command Line:</strong> {{ selectedProcess.cmdline }}
        </div>
        <div v-if="selectedProcess.maps" class="maps-container">
          <h5>Memory Maps:</h5>
          <pre>{{ selectedProcess.maps }}</pre>
        </div>
      </div>
    </div>

    <div class="stats">
      <span v-if="isConnected">
        {{ processes.length }} processes
      </span>
      <span v-if="lastRefresh">
        Last refresh: {{ lastRefresh }}
      </span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import { useQGA } from '../composables/useQGA'

const {
  isConnected,
  isConnecting,
  processes,
  error,
  connect,
  disconnect,
  getProcessList,
  getProcessDetails,
  startAutoRefresh,
  stopAutoRefresh,
  startMapsAutoRefresh,
  stopMapsAutoRefresh
} = useQGA()

const isLoading = ref(false)
const selectedPid = ref<number | null>(null)
const selectedProcess = ref<any>(null)
const searchQuery = ref('')
const autoRefresh = ref(false)
const lastRefresh = ref<string>('')

// Filter processes based on search
const filteredProcesses = computed(() => {
  if (!searchQuery.value) return processes.value

  const query = searchQuery.value.toLowerCase()
  return processes.value.filter(p =>
    p.comm.toLowerCase().includes(query) ||
    p.pid.toString().includes(query)
  )
})

// Refresh process list
async function refresh() {
  isLoading.value = true
  try {
    await getProcessList()
    lastRefresh.value = new Date().toLocaleTimeString()
  } finally {
    isLoading.value = false
  }
}

// Select a process
async function selectProcess(pid: number) {
  selectedPid.value = pid
  selectedProcess.value = await getProcessDetails(pid)

  // Start auto-refreshing maps for this process (every 2 seconds)
  startMapsAutoRefresh(pid, 2000)
}

// Clean up when selecting different process
watch(selectedPid, (newPid, oldPid) => {
  if (oldPid && newPid !== oldPid) {
    stopMapsAutoRefresh()
  }
})

// Show memory maps for a process
async function showMaps(pid: number) {
  await selectProcess(pid)
}

// Toggle auto-refresh
function toggleAutoRefresh() {
  if (autoRefresh.value) {
    startAutoRefresh(5000) // Refresh PID list every 5 seconds
  } else {
    stopAutoRefresh()
  }
}

// Auto-load processes on connect
watch(isConnected, (connected) => {
  if (connected) {
    refresh()
  }
})
</script>

<style scoped>
.process-explorer {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: #1e1e1e;
  color: #d4d4d4;
}

.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px;
  background: #2d2d30;
  border-bottom: 1px solid #3e3e42;
}

.header h3 {
  margin: 0;
  font-size: 16px;
}

.controls {
  display: flex;
  gap: 10px;
  align-items: center;
}

button {
  background: #0e639c;
  color: white;
  border: none;
  padding: 5px 12px;
  border-radius: 3px;
  cursor: pointer;
  font-size: 12px;
}

button:hover:not(:disabled) {
  background: #1177bb;
}

button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.error {
  background: #5a1d1d;
  color: #f48771;
  padding: 10px;
  margin: 10px;
  border-radius: 3px;
}

.content {
  display: flex;
  flex: 1;
  overflow: hidden;
}

.process-list {
  flex: 1;
  overflow-y: auto;
  border-right: 1px solid #3e3e42;
}

table {
  width: 100%;
  border-collapse: collapse;
}

thead {
  position: sticky;
  top: 0;
  background: #252526;
}

th {
  text-align: left;
  padding: 8px;
  border-bottom: 1px solid #3e3e42;
  font-size: 12px;
  font-weight: normal;
  color: #cccccc;
}

tbody tr {
  cursor: pointer;
}

tbody tr:hover {
  background: #2a2d2e;
}

tbody tr.selected {
  background: #094771;
}

td {
  padding: 6px 8px;
  font-size: 12px;
  border-bottom: 1px solid #1e1e1e;
}

.process-details {
  flex: 1;
  padding: 15px;
  overflow-y: auto;
}

.process-details h4 {
  margin: 0 0 15px 0;
  color: #cccccc;
}

.detail-row {
  margin: 10px 0;
  font-size: 12px;
}

.maps-container {
  margin-top: 20px;
}

.maps-container h5 {
  margin: 0 0 10px 0;
  color: #cccccc;
}

pre {
  background: #1e1e1e;
  border: 1px solid #3e3e42;
  padding: 10px;
  border-radius: 3px;
  font-size: 11px;
  overflow-x: auto;
  max-height: 400px;
}

.stats {
  padding: 5px 10px;
  background: #252526;
  border-top: 1px solid #3e3e42;
  font-size: 11px;
  color: #969696;
  display: flex;
  justify-content: space-between;
}

label {
  font-size: 12px;
  display: flex;
  align-items: center;
  gap: 5px;
}

input[type="checkbox"] {
  margin: 0;
}

.refresh-indicator {
  font-size: 10px;
  color: #969696;
  margin-left: 10px;
  animation: pulse 2s infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 0.5; }
  50% { opacity: 1; }
}
</style>