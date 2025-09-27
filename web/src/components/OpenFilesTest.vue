<template>
  <div class="open-files-test">
    <h2>Open Files Discovery Test</h2>

    <div class="controls">
      <button @click="runDiscovery" :disabled="running">
        {{ running ? 'Running...' : 'Discover Open Files' }}
      </button>

      <button @click="clearResults" :disabled="running">
        Clear
      </button>
    </div>

    <div v-if="error" class="error">
      Error: {{ error }}
    </div>

    <div v-if="results" class="results">
      <h3>Results</h3>

      <div class="summary">
        <p>Found {{ results.processCount }} processes</p>
        <p>Found {{ results.uniqueFiles }} unique open files</p>
        <p>Total file size: {{ formatSize(results.totalSize) }}</p>
      </div>

      <div v-if="results.files.length > 0" class="files-list">
        <h4>Open Files:</h4>
        <table>
          <thead>
            <tr>
              <th>Inode</th>
              <th>Size</th>
              <th>Mode</th>
              <th>Processes</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="file in results.files" :key="file.inodeAddr">
              <td>{{ file.ino }}</td>
              <td>{{ formatSize(Number(file.size)) }}</td>
              <td>{{ formatMode(file.mode) }}</td>
              <td>{{ file.processes.join(', ') }}</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div class="known-files">
        <h4>Known Files Check:</h4>
        <ul>
          <li v-for="check in knownFilesCheck" :key="check.ino">
            {{ check.found ? '✓' : '✗' }} Inode {{ check.ino }} ({{ check.name }})
          </li>
        </ul>
      </div>
    </div>

    <div v-if="log.length > 0" class="log">
      <h4>Log:</h4>
      <pre>{{ log.join('\n') }}</pre>
    </div>
  </div>
</template>

<script lang="ts">
import { defineComponent, ref } from 'vue';
import { PagedMemory } from '../paged-memory';
import { KernelMem, OFFSETS } from '../kernel-mem';
import { PagedKernelDiscovery } from '../kernel-discovery-paged';

interface FileInfo {
  inodeAddr: string;
  ino: string;
  size: bigint;
  mode: number;
  processes: string[];
}

interface Results {
  processCount: number;
  uniqueFiles: number;
  totalSize: bigint;
  files: FileInfo[];
}

// Known addresses for our test system
const INIT_TASK = BigInt('0xffff8000838e2880');

export default defineComponent({
  name: 'OpenFilesTest',
  props: {
    memory: {
      type: Object as () => PagedMemory,
      required: true
    },
    kernelDiscovery: {
      type: Object,
      default: null
    },
    discoveryOutput: {
      type: Object,
      default: null
    }
  },
  setup(props) {
    const running = ref(false);
    const error = ref<string | null>(null);
    const results = ref<Results | null>(null);
    const log = ref<string[]>([]);
    const knownFilesCheck = ref<Array<{ino: number, name: string, found: boolean}>>([]);

    const logMsg = (msg: string) => {
      console.log(msg);
      log.value.push(msg);
    };

    const clearResults = () => {
      error.value = null;
      results.value = null;
      log.value = [];
      knownFilesCheck.value = [];
    };

    const formatSize = (bytes: number): string => {
      if (bytes < 1024) return `${bytes} B`;
      if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
      return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    };

    const formatMode = (mode: number): string => {
      const types: Record<number, string> = {
        0x8000: 'REG',
        0x4000: 'DIR',
        0x2000: 'CHR',
        0x6000: 'BLK',
        0x1000: 'FIFO',
        0xA000: 'LNK',
        0xC000: 'SOCK'
      };

      const type = mode & 0xF000;
      const typeStr = types[type] || 'UNK';
      const perms = (mode & 0x1FF).toString(8).padStart(3, '0');
      return `${typeStr} ${perms}`;
    };

    const runDiscovery = async () => {
      clearResults();
      running.value = true;

      try {
        logMsg('Starting open files discovery...');

        // Create kernel memory helper
        const kmem = new KernelMem(props.memory);

        // Use already-discovered data if available
        let discoveryResult = props.discoveryOutput;

        if (!discoveryResult || !discoveryResult.swapperPgDir) {
          // Only run discovery if we don't have it already
          logMsg('No existing discovery data, finding kernel PGD...');
          const kernelDiscovery = new PagedKernelDiscovery(props.memory);
          discoveryResult = await kernelDiscovery.discover(props.memory.getTotalSize());
        } else {
          logMsg('Using existing discovery data');
        }

        if (discoveryResult && discoveryResult.swapperPgDir) {
          // swapperPgDir is the kernel PGD physical address
          kmem.setKernelPgd(discoveryResult.swapperPgDir);
          logMsg(`Using kernel PGD at PA 0x${discoveryResult.swapperPgDir.toString(16)}`);
        } else {
          logMsg('Warning: Could not find kernel PGD, using default 0x82a12000');
        }

        // Use existing processes or walk task list to find them
        let processes = [];

        if (discoveryResult && discoveryResult.processes && discoveryResult.processes.length > 0) {
          logMsg('Using existing processes from discovery...');
          // Convert discovery processes to the format we need
          processes = discoveryResult.processes
            .filter(p => !p.isKernelThread && p.name && !p.name.startsWith('['))
            .map(p => ({
              addr: BigInt(p.taskStruct),
              pid: p.pid,
              name: p.name,
              files: BigInt(0) // We'll read this fresh
            }));

          // Read files pointer for each process
          for (const proc of processes) {
            const files = kmem.readU64(proc.addr + BigInt(OFFSETS['task.files']));
            if (files && files > 0n) {
              proc.files = files;
            }
          }

          processes = processes.filter(p => p.files && p.files > 0n);
          logMsg(`Found ${processes.length} user processes with files`);
        } else {
          // Fall back to walking task list
          logMsg('Walking task list...');
          const taskAddrs = kmem.walkList(INIT_TASK + BigInt(OFFSETS['task.tasks']), OFFSETS['task.tasks'], 500);

          logMsg(`Found ${taskAddrs.length} tasks`);

          for (const taskAddr of taskAddrs) {
            const pid = kmem.readU32(taskAddr + BigInt(OFFSETS['task.pid']));
            if (!pid || pid <= 0) continue;

            const name = kmem.readString(taskAddr + BigInt(OFFSETS['task.comm']));
            if (!name || name.startsWith('[')) continue; // Skip kernel threads

            const files = kmem.readU64(taskAddr + BigInt(OFFSETS['task.files']));
            if (files && files > 0n) {
              processes.push({ addr: taskAddr, pid, name, files });
            }
          }

          logMsg(`Found ${processes.length} user processes with files`);
        }

        // Discover open files
        const openFiles = new Map<bigint, FileInfo>();

        for (const proc of processes) {
          // Read fdtable
          const fdtPtr = kmem.readU64(proc.files + BigInt(OFFSETS['files.fdt']));
          if (!fdtPtr) continue;

          const maxFds = kmem.readU32(fdtPtr + BigInt(OFFSETS['fdt.max_fds']));
          const fdArrayPtr = kmem.readU64(fdtPtr + BigInt(OFFSETS['fdt.fd']));

          if (!maxFds || !fdArrayPtr) continue;

          const checkFds = Math.min(maxFds, 100);
          let foundInProc = 0;

          for (let fd = 0; fd < checkFds; fd++) {
            const filePtr = kmem.readU64(fdArrayPtr + BigInt(fd * 8));
            if (!filePtr || filePtr === 0n) continue;

            const inodePtr = kmem.readU64(filePtr + BigInt(OFFSETS['file.inode']));
            if (!inodePtr || inodePtr === 0n) continue;

            foundInProc++;

            if (!openFiles.has(inodePtr)) {
              const ino = kmem.readU64(inodePtr + BigInt(OFFSETS['inode.ino'])) || 0n;
              const size = kmem.readU64(inodePtr + BigInt(OFFSETS['inode.size'])) || 0n;
              const mode = kmem.readU32(inodePtr + BigInt(OFFSETS['inode.mode'])) || 0;

              // Debug logging for first few files
              if (openFiles.size < 3) {
                logMsg(`    fd=${fd}: filePtr=0x${filePtr.toString(16)}, inodePtr=0x${inodePtr.toString(16)}`);
                logMsg(`      ino=${ino}, size=${size}, mode=0x${mode.toString(16)}`);
              }

              // Only add files that have valid data
              if (ino > 0n || size > 0n || mode > 0) {
                openFiles.set(inodePtr, {
                  inodeAddr: `0x${inodePtr.toString(16)}`,
                  ino: ino.toString(),
                  size: size,
                  mode: mode,
                  processes: [`${proc.name}[${proc.pid}]`]
                });
              }
            } else {
              openFiles.get(inodePtr)!.processes.push(`${proc.name}[${proc.pid}]`);
            }
          }

          if (foundInProc > 0) {
            logMsg(`  ${proc.name}[${proc.pid}]: ${foundInProc} open files`);
          }
        }

        // Sort files by inode number
        const filesArray = Array.from(openFiles.values()).sort((a, b) => {
          const aNum = BigInt(a.ino);
          const bNum = BigInt(b.ino);
          if (aNum < bNum) return -1;
          if (aNum > bNum) return 1;
          return 0;
        });

        // Calculate total size
        const totalSize = filesArray.reduce((sum, f) => sum + f.size, 0n);

        // Check for known files (from /proc/meminfo)
        const knownInodes = [
          { ino: 272643, name: 'libpcre2-8.so' },
          { ino: 262017, name: 'bash' },
          { ino: 267562, name: 'systemctl' }
        ];

        knownFilesCheck.value = knownInodes.map(known => ({
          ...known,
          found: filesArray.some(f => f.ino === known.ino.toString())
        }));

        results.value = {
          processCount: processes.length,
          uniqueFiles: openFiles.size,
          totalSize: totalSize,
          files: filesArray.slice(0, 100) // Limit display to first 100
        };

        logMsg(`\nDiscovery complete: ${openFiles.size} unique files found`);

      } catch (e) {
        error.value = e instanceof Error ? e.message : String(e);
        logMsg(`Error: ${error.value}`);
      } finally {
        running.value = false;
      }
    };

    return {
      running,
      error,
      results,
      log,
      knownFilesCheck,
      runDiscovery,
      clearResults,
      formatSize,
      formatMode
    };
  }
});
</script>

<style scoped>
.open-files-test {
  padding: 20px;
  max-width: 1200px;
  margin: 0 auto;
}

h2 {
  margin-bottom: 20px;
}

.controls {
  margin-bottom: 20px;
}

.controls button {
  margin-right: 10px;
  padding: 8px 16px;
  font-size: 14px;
  cursor: pointer;
}

.controls button:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.error {
  background: #ffebee;
  color: #c62828;
  padding: 12px;
  margin: 20px 0;
  border-radius: 4px;
}

.summary {
  background: #f5f5f5;
  padding: 12px;
  margin: 20px 0;
  border-radius: 4px;
}

.summary p {
  margin: 4px 0;
}

.files-list {
  margin: 20px 0;
}

.files-list table {
  width: 100%;
  border-collapse: collapse;
}

.files-list th {
  background: #e0e0e0;
  padding: 8px;
  text-align: left;
  border-bottom: 2px solid #999;
}

.files-list td {
  padding: 6px 8px;
  border-bottom: 1px solid #ddd;
}

.known-files {
  margin: 20px 0;
  padding: 12px;
  background: #f5f5f5;
  border-radius: 4px;
}

.known-files ul {
  list-style: none;
  padding: 0;
}

.known-files li {
  padding: 4px 0;
}

.log {
  margin-top: 20px;
  padding: 12px;
  background: #f5f5f5;
  border-radius: 4px;
}

.log pre {
  margin: 8px 0;
  font-size: 12px;
  overflow-x: auto;
}
</style>