<template>
  <div class="kernel-discovery-report">
    <!-- Control Bar -->
    <div class="control-bar">
      <button
        @click="runDiscovery"
        :disabled="isRunning"
        class="discovery-btn"
      >
        <span v-if="!isRunning">🔍 Run Kernel Discovery</span>
        <span v-else>⏳ Discovering... ({{ progressPercent }}%)</span>
      </button>

      <button
        v-if="discoveryComplete"
        @click="generateReport"
        class="report-btn"
      >
        📊 Generate Report
      </button>

      <button
        v-if="reportGenerated"
        @click="exportReport"
        class="export-btn"
      >
        💾 Export Report
      </button>
    </div>

    <!-- Progress Bar -->
    <div v-if="isRunning" class="progress-bar">
      <div class="progress-fill" :style="{width: progressPercent + '%'}"></div>
      <span class="progress-text">{{ currentPhase }}</span>
    </div>

    <!-- Quick Stats -->
    <div v-if="discoveryComplete" class="quick-stats">
      <div class="stat-card">
        <h3>Processes</h3>
        <div class="stat-value">{{ stats.totalProcesses }}</div>
        <div class="stat-detail">
          {{ stats.userProcesses }} user / {{ stats.kernelThreads }} kernel
        </div>
      </div>

      <div class="stat-card">
        <h3>Page Tables</h3>
        <div class="stat-value">{{ stats.totalPTEs + stats.kernelPTEs }}</div>
        <div class="stat-detail">
          {{ stats.totalPTEs }} process / {{ stats.kernelPTEs }} kernel
        </div>
      </div>

      <div class="stat-card">
        <h3>Memory Pages</h3>
        <div class="stat-value">{{ stats.uniquePages }}</div>
        <div class="stat-detail">
          {{ stats.sharedPages }} shared / {{ stats.zeroPages }} zero
        </div>
      </div>
    </div>

    <!-- Detailed Report -->
    <div v-if="reportGenerated" class="report-container">
      <div class="report-header">
        <h2>Kernel Memory Discovery Report</h2>
        <div class="report-meta">
          Generated: {{ reportTimestamp }}<br>
          Memory File: {{ memoryFileName }}<br>
          File Size: {{ formatBytes(memoryFileSize) }}
        </div>
      </div>

      <!-- Process List Section -->
      <div class="report-section">
        <h3>Process List ({{ processes.length }} total)</h3>
        <div class="process-table-container">
          <table class="process-table">
            <thead>
              <tr>
                <th @click="sortBy('pid')">PID ↕</th>
                <th @click="sortBy('name')">Name ↕</th>
                <th>Type</th>
                <th @click="sortBy('ptes')">PTEs ↕</th>
                <th @click="sortBy('sections')">Sections ↕</th>
                <th>Task Struct</th>
                <th>Actions</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="proc in sortedProcesses" :key="proc.pid">
                <td>{{ proc.pid }}</td>
                <td class="proc-name">{{ proc.name }}</td>
                <td>
                  <span :class="proc.isKernelThread ? 'kernel-badge' : 'user-badge'">
                    {{ proc.isKernelThread ? 'Kernel' : 'User' }}
                  </span>
                </td>
                <td>{{ proc.ptes?.length || 0 }}</td>
                <td>{{ proc.sections?.length || 0 }}</td>
                <td class="mono">0x{{ proc.taskStruct?.toString(16) || '0' }}</td>
                <td>
                  <button @click="viewProcessDetail(proc)" class="btn-small">
                    View
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <!-- Memory Sections Summary -->
      <div class="report-section">
        <h3>Memory Section Distribution</h3>
        <div class="section-stats">
          <div class="section-chart">
            <canvas ref="sectionChart"></canvas>
          </div>
          <div class="section-legend">
            <div v-for="type in sectionTypes" :key="type.name" class="legend-item">
              <span class="legend-color" :style="{background: type.color}"></span>
              <span>{{ type.name }}: {{ type.count }} sections ({{ formatBytes(type.size) }})</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Shared Pages Analysis -->
      <div class="report-section">
        <h3>Shared Memory Analysis</h3>
        <div class="shared-analysis">
          <p>{{ stats.sharedPages }} pages are shared between multiple processes (likely library code)</p>

          <div class="shared-table-container">
            <table class="shared-table">
              <thead>
                <tr>
                  <th>Physical Page</th>
                  <th>Shared By</th>
                  <th>Process Names</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(pids, page) in topSharedPages" :key="page">
                  <td class="mono">{{ page }}</td>
                  <td>{{ pids.length }} processes</td>
                  <td>{{ getProcessNames(pids).join(', ') }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <!-- Kernel Page Tables -->
      <div class="report-section">
        <h3>Kernel Page Tables</h3>
        <div class="kernel-info">
          <p><strong>swapper_pg_dir:</strong> <span class="mono">0x082c00000</span></p>
          <p><strong>Kernel PTEs:</strong> {{ stats.kernelPTEs }}</p>

          <div class="kernel-sample">
            <h4>Sample Kernel Mappings:</h4>
            <table class="pte-table">
              <thead>
                <tr>
                  <th>Virtual Address</th>
                  <th>Physical Address</th>
                  <th>Permissions</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="pte in kernelPtesSample" :key="pte.va">
                  <td class="mono">{{ pte.va }}</td>
                  <td class="mono">{{ pte.pa }}</td>
                  <td>{{ pte.rwx }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <!-- Export Options -->
      <div class="report-section">
        <h3>Export Options</h3>
        <div class="export-options">
          <button @click="exportJSON" class="export-btn">
            📄 Export as JSON
          </button>
          <button @click="exportCSV" class="export-btn">
            📊 Export Process List as CSV
          </button>
          <button @click="exportHTML" class="export-btn">
            🌐 Export Full HTML Report
          </button>
          <button @click="printReport" class="export-btn">
            🖨️ Print Report
          </button>
        </div>
      </div>
    </div>

    <!-- Process Detail Modal -->
    <div v-if="selectedProcess" class="modal" @click.self="closeProcessDetail">
      <div class="modal-content">
        <h3>Process Detail: {{ selectedProcess.name }} (PID {{ selectedProcess.pid }})</h3>

        <div class="process-details">
          <h4>Memory Sections:</h4>
          <table class="section-table">
            <thead>
              <tr>
                <th>Type</th>
                <th>Virtual Range</th>
                <th>Size</th>
                <th>Pages</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="section in selectedProcessSections" :key="section.range">
                <td>{{ section.type }}</td>
                <td class="mono">{{ section.range }}</td>
                <td>{{ section.size }}</td>
                <td>{{ section.pages }}</td>
              </tr>
            </tbody>
          </table>

          <h4>Sample PTEs:</h4>
          <table class="pte-table">
            <thead>
              <tr>
                <th>Virtual</th>
                <th>Physical</th>
                <th>Permissions</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="pte in selectedProcessPTEs" :key="pte.va">
                <td class="mono">{{ pte.va }}</td>
                <td class="mono">{{ pte.pa }}</td>
                <td>{{ pte.rwx }}</td>
              </tr>
            </tbody>
          </table>
        </div>

        <button @click="closeProcessDetail" class="close-btn">Close</button>
      </div>
    </div>
  </div>
</template>

<script lang="ts">
import { defineComponent, ref, computed, onMounted } from 'vue';
import { KernelDiscovery, formatDiscoveryOutput } from '../kernel-discovery';
import type { ProcessInfo, DiscoveryOutput } from '../kernel-discovery';

export default defineComponent({
  name: 'KernelDiscoveryReport',

  props: {
    memoryFile: ArrayBuffer,
    fileName: String,
  },

  setup(props) {
    // State
    const isRunning = ref(false);
    const discoveryComplete = ref(false);
    const reportGenerated = ref(false);
    const currentPhase = ref('');
    const progressPercent = ref(0);
    const reportTimestamp = ref('');

    // Discovery data
    const discoveryOutput = ref<DiscoveryOutput | null>(null);
    const processes = ref<ProcessInfo[]>([]);
    const stats = ref({
      totalProcesses: 0,
      kernelThreads: 0,
      userProcesses: 0,
      totalPTEs: 0,
      kernelPTEs: 0,
      uniquePages: 0,
      sharedPages: 0,
      zeroPages: 0,
    });

    // UI state
    const sortField = ref('pid');
    const sortAscending = ref(true);
    const selectedProcess = ref<ProcessInfo | null>(null);
    const selectedProcessPTEs = ref([]);
    const selectedProcessSections = ref([]);

    // Computed
    const memoryFileName = computed(() => props.fileName || 'memory.dump');
    const memoryFileSize = computed(() => props.memoryFile?.byteLength || 0);

    const sortedProcesses = computed(() => {
      const procs = [...processes.value];
      procs.sort((a, b) => {
        let aVal = a[sortField.value];
        let bVal = b[sortField.value];

        // Handle array properties (ptes and sections)
        if (Array.isArray(aVal)) {
          aVal = aVal.length;
        }
        if (Array.isArray(bVal)) {
          bVal = bVal.length;
        }

        if (typeof aVal === 'string') {
          aVal = aVal.toLowerCase();
          bVal = bVal.toLowerCase();
        }

        if (sortAscending.value) {
          return aVal < bVal ? -1 : aVal > bVal ? 1 : 0;
        } else {
          return aVal > bVal ? -1 : aVal < bVal ? 1 : 0;
        }
      });
      return procs;
    });

    const topSharedPages = computed(() => {
      if (!discoveryOutput.value) return {};

      const sorted = Array.from(discoveryOutput.value.pageToPids.entries())
        .filter(([_, pids]) => pids.size > 1)
        .sort((a, b) => b[1].size - a[1].size)
        .slice(0, 10);

      const result = {};
      for (const [page, pids] of sorted) {
        result[`0x${page.toString(16)}`] = Array.from(pids);
      }
      return result;
    });

    const kernelPtesSample = computed(() => {
      if (!discoveryOutput.value) return [];

      return discoveryOutput.value.kernelPtes.slice(0, 10).map(pte => ({
        va: `0x${pte.va.toString(16)}`,
        pa: `0x${pte.pa.toString(16)}`,
        rwx: `${pte.r ? 'r' : '-'}${pte.w ? 'w' : '-'}${pte.x ? 'x' : '-'}`,
      }));
    });

    const sectionTypes = computed(() => {
      const types = new Map<string, {count: number, size: number}>();

      if (discoveryOutput.value) {
        for (const sections of discoveryOutput.value.sectionsByPid.values()) {
          for (const section of sections) {
            if (!types.has(section.type)) {
              types.set(section.type, {count: 0, size: 0});
            }
            const stat = types.get(section.type)!;
            stat.count++;
            stat.size += section.size;
          }
        }
      }

      const colors = {
        code: '#4CAF50',
        data: '#2196F3',
        heap: '#FF9800',
        stack: '#9C27B0',
        library: '#00BCD4',
        kernel: '#F44336',
      };

      return Array.from(types.entries()).map(([name, stats]) => ({
        name,
        count: stats.count,
        size: stats.size,
        color: colors[name] || '#757575',
      }));
    });

    // Methods
    const runDiscovery = async () => {
      if (!props.memoryFile) {
        alert('No memory file loaded');
        return;
      }

      isRunning.value = true;
      progressPercent.value = 0;

      try {
        // Simulate progress updates
        const phases = [
          'Finding processes...',
          'Discovering PTEs...',
          'Mapping kernel memory...',
          'Analyzing sections...',
          'Building reverse mappings...',
        ];

        for (let i = 0; i < phases.length; i++) {
          currentPhase.value = phases[i];
          progressPercent.value = (i / phases.length) * 100;
          await new Promise(resolve => setTimeout(resolve, 100));
        }

        // Run actual discovery
        const discovery = new KernelDiscovery(props.memoryFile);
        discoveryOutput.value = await discovery.discover();

        // Update UI state
        processes.value = discoveryOutput.value.processes;
        stats.value = discoveryOutput.value.stats;

        progressPercent.value = 100;
        currentPhase.value = 'Discovery complete!';
        discoveryComplete.value = true;

        setTimeout(() => {
          isRunning.value = false;
        }, 1000);

      } catch (error) {
        console.error('Discovery failed:', error);
        alert('Discovery failed: ' + error.message);
        isRunning.value = false;
      }
    };

    const generateReport = () => {
      reportTimestamp.value = new Date().toLocaleString();
      reportGenerated.value = true;
    };

    const exportReport = () => {
      exportHTML();
    };

    const exportJSON = () => {
      if (!discoveryOutput.value) return;

      const formatted = formatDiscoveryOutput(discoveryOutput.value);
      const json = JSON.stringify(formatted, null, 2);
      const blob = new Blob([json], { type: 'application/json' });
      const url = URL.createObjectURL(blob);

      const a = document.createElement('a');
      a.href = url;
      a.download = `kernel-discovery-${Date.now()}.json`;
      a.click();

      URL.revokeObjectURL(url);
    };

    const exportCSV = () => {
      if (!processes.value.length) return;

      const headers = ['PID', 'Name', 'Type', 'PTEs', 'Sections', 'Task Struct'];
      const rows = processes.value.map(p => [
        p.pid,
        p.name,
        p.isKernelThread ? 'Kernel' : 'User',
        p.ptes.length,
        p.sections.length,
        `0x${p.taskStruct.toString(16)}`,
      ]);

      const csv = [
        headers.join(','),
        ...rows.map(r => r.join(','))
      ].join('\n');

      const blob = new Blob([csv], { type: 'text/csv' });
      const url = URL.createObjectURL(blob);

      const a = document.createElement('a');
      a.href = url;
      a.download = `process-list-${Date.now()}.csv`;
      a.click();

      URL.revokeObjectURL(url);
    };

    const exportHTML = () => {
      const reportEl = document.querySelector('.report-container');
      if (!reportEl) return;

      const html = `
        <!DOCTYPE html>
        <html>
        <head>
          <title>Kernel Discovery Report</title>
          <style>
            body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; padding: 20px; }
            table { border-collapse: collapse; width: 100%; margin: 20px 0; }
            th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
            th { background: #f5f5f5; }
            .mono { font-family: monospace; }
            .stat-card { display: inline-block; margin: 10px; padding: 15px; border: 1px solid #ddd; }
            .stat-value { font-size: 24px; font-weight: bold; }
            h2, h3 { color: #333; }
          </style>
        </head>
        <body>
          ${reportEl.innerHTML}
        </body>
        </html>
      `;

      const blob = new Blob([html], { type: 'text/html' });
      const url = URL.createObjectURL(blob);

      const a = document.createElement('a');
      a.href = url;
      a.download = `kernel-report-${Date.now()}.html`;
      a.click();

      URL.revokeObjectURL(url);
    };

    const printReport = () => {
      window.print();
    };

    const sortBy = (field: string) => {
      if (sortField.value === field) {
        sortAscending.value = !sortAscending.value;
      } else {
        sortField.value = field;
        sortAscending.value = true;
      }
    };

    const viewProcessDetail = (proc: ProcessInfo) => {
      selectedProcess.value = proc;

      if (discoveryOutput.value) {
        selectedProcessPTEs.value = discoveryOutput.value.ptesByPid.get(proc.pid)
          ?.slice(0, 20)
          .map(pte => ({
            va: `0x${pte.va.toString(16)}`,
            pa: `0x${pte.pa.toString(16)}`,
            rwx: `${pte.r ? 'r' : '-'}${pte.w ? 'w' : '-'}${pte.x ? 'x' : '-'}`,
          })) || [];

        selectedProcessSections.value = discoveryOutput.value.sectionsByPid.get(proc.pid)
          ?.map(s => ({
            type: s.type,
            range: `0x${s.startVa.toString(16)}-0x${s.endVa.toString(16)}`,
            size: `${(s.size / 1024).toFixed(1)}KB`,
            pages: s.pages,
          })) || [];
      }
    };

    const closeProcessDetail = () => {
      selectedProcess.value = null;
    };

    const getProcessNames = (pids: number[]) => {
      return pids.map(pid => {
        if (pid === 0) return 'kernel';
        const proc = processes.value.find(p => p.pid === pid);
        return proc?.name || `PID ${pid}`;
      });
    };

    const formatBytes = (bytes: number) => {
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
      return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB';
    };

    return {
      isRunning,
      discoveryComplete,
      reportGenerated,
      currentPhase,
      progressPercent,
      reportTimestamp,
      processes,
      stats,
      sortedProcesses,
      topSharedPages,
      kernelPtesSample,
      sectionTypes,
      selectedProcess,
      selectedProcessPTEs,
      selectedProcessSections,
      memoryFileName,
      memoryFileSize,

      runDiscovery,
      generateReport,
      exportReport,
      exportJSON,
      exportCSV,
      exportHTML,
      printReport,
      sortBy,
      viewProcessDetail,
      closeProcessDetail,
      getProcessNames,
      formatBytes,
    };
  },
});
</script>

<style scoped>
.kernel-discovery-report {
  padding: 20px;
}

.control-bar {
  display: flex;
  gap: 10px;
  margin-bottom: 20px;
}

.discovery-btn, .report-btn, .export-btn {
  padding: 10px 20px;
  font-size: 16px;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  background: #4CAF50;
  color: white;
}

.discovery-btn:disabled {
  background: #ccc;
  cursor: not-allowed;
}

.report-btn {
  background: #2196F3;
}

.export-btn {
  background: #FF9800;
}

.progress-bar {
  height: 30px;
  background: #f5f5f5;
  border-radius: 4px;
  overflow: hidden;
  position: relative;
  margin-bottom: 20px;
}

.progress-fill {
  height: 100%;
  background: linear-gradient(90deg, #4CAF50, #8BC34A);
  transition: width 0.3s ease;
}

.progress-text {
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  font-weight: bold;
}

.quick-stats {
  display: flex;
  gap: 20px;
  margin-bottom: 30px;
}

.stat-card {
  flex: 1;
  padding: 20px;
  background: white;
  border: 1px solid #ddd;
  border-radius: 8px;
  box-shadow: 0 2px 4px rgba(0,0,0,0.1);
}

.stat-card h3 {
  margin: 0 0 10px 0;
  color: #666;
  font-size: 14px;
  text-transform: uppercase;
}

.stat-value {
  font-size: 32px;
  font-weight: bold;
  color: #333;
}

.stat-detail {
  color: #666;
  font-size: 14px;
  margin-top: 5px;
}

.report-container {
  background: white;
  border: 1px solid #ddd;
  border-radius: 8px;
  padding: 30px;
}

.report-header {
  border-bottom: 2px solid #333;
  padding-bottom: 20px;
  margin-bottom: 30px;
}

.report-header h2 {
  margin: 0 0 10px 0;
}

.report-meta {
  color: #666;
  font-size: 14px;
}

.report-section {
  margin-bottom: 40px;
}

.report-section h3 {
  margin-bottom: 20px;
  color: #333;
}

.process-table, .shared-table, .pte-table, .section-table {
  width: 100%;
  border-collapse: collapse;
}

.process-table th,
.shared-table th,
.pte-table th,
.section-table th {
  background: #f5f5f5;
  padding: 10px;
  text-align: left;
  border: 1px solid #ddd;
  cursor: pointer;
}

.process-table td,
.shared-table td,
.pte-table td,
.section-table td {
  padding: 8px;
  border: 1px solid #ddd;
}

.mono {
  font-family: 'Courier New', monospace;
  font-size: 12px;
}

.proc-name {
  font-weight: 500;
}

.kernel-badge, .user-badge {
  padding: 2px 8px;
  border-radius: 3px;
  font-size: 12px;
  font-weight: bold;
}

.kernel-badge {
  background: #FFF3E0;
  color: #E65100;
}

.user-badge {
  background: #E3F2FD;
  color: #0D47A1;
}

.btn-small {
  padding: 4px 8px;
  font-size: 12px;
  background: #4CAF50;
  color: white;
  border: none;
  border-radius: 3px;
  cursor: pointer;
}

.btn-small:hover {
  background: #45a049;
}

.section-stats {
  display: flex;
  gap: 30px;
}

.section-chart {
  flex: 1;
  height: 300px;
}

.section-legend {
  flex: 1;
}

.legend-item {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 10px;
}

.legend-color {
  width: 20px;
  height: 20px;
  border-radius: 3px;
}

.export-options {
  display: flex;
  gap: 10px;
  flex-wrap: wrap;
}

.modal {
  position: fixed;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.modal-content {
  background: white;
  padding: 30px;
  border-radius: 8px;
  max-width: 80%;
  max-height: 80%;
  overflow-y: auto;
}

.close-btn {
  margin-top: 20px;
  padding: 10px 20px;
  background: #f44336;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
}

.process-details h4 {
  margin-top: 20px;
  margin-bottom: 10px;
  color: #333;
}

@media print {
  .control-bar,
  .export-options,
  .btn-small,
  .close-btn {
    display: none;
  }
}
</style>