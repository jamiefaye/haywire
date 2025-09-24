<template>
  <transition name="tooltip-fade">
    <div
      v-if="visible && pageInfo"
      class="kernel-page-tooltip"
      :style="tooltipStyle"
      @mouseenter="handleMouseEnter"
      @mouseleave="handleMouseLeave"
    >
      <!-- Header with page address -->
      <div class="tooltip-header">
        <div class="page-address">
          Physical Page: {{ formatAddress(pageInfo.physicalAddress) }}
        </div>
        <div class="page-badges">
          <span v-if="pageInfo.isKernel" class="badge kernel">Kernel</span>
          <span v-if="pageInfo.isShared" class="badge shared">Shared</span>
          <span v-if="pageInfo.isZero" class="badge zero">Zero</span>
        </div>
      </div>

      <!-- References section -->
      <div class="tooltip-body">
        <div v-if="pageInfo.notFound" class="no-references">
          No kernel page information found for this address
        </div>
        <div v-else-if="pageInfo.references.length === 0" class="no-references">
          No process references found
        </div>

        <div v-else class="references-list">
          <div class="reference-count">
            {{ pageInfo.references.length }} reference{{ pageInfo.references.length > 1 ? 's' : '' }}
          </div>

          <!-- Group references by process -->
          <div v-for="(group, pid) in groupedReferences" :key="pid" class="process-group">
            <div class="process-header">
              <span class="process-name">{{ group[0].processName }}</span>
              <span class="process-pid">(PID {{ pid }})</span>
            </div>

            <div class="reference-items">
              <div v-for="(ref, index) in group" :key="index" class="reference-item">
                <span class="ref-type" :class="ref.type">
                  {{ ref.type === 'pte' ? 'PTE' : 'Section' }}
                </span>

                <span v-if="ref.sectionType" class="section-type">
                  {{ ref.sectionType }}
                </span>

                <span class="virtual-addr">
                  VA: {{ formatAddress(ref.virtualAddress) }}
                </span>

                <span class="permissions">
                  {{ ref.permissions }}
                </span>

                <span v-if="ref.size" class="size">
                  ({{ formatBytes(ref.size) }})
                </span>
              </div>
            </div>
          </div>
        </div>

        <!-- Summary statistics -->
        <div v-if="showSummary" class="tooltip-summary">
          <div class="summary-item">
            <span class="label">Unique Processes:</span>
            <span class="value">{{ uniqueProcessCount }}</span>
          </div>
          <div v-if="pteCount > 0" class="summary-item">
            <span class="label">PTEs:</span>
            <span class="value">{{ pteCount }}</span>
          </div>
          <div v-if="sectionCount > 0" class="summary-item">
            <span class="label">Sections:</span>
            <span class="value">{{ sectionCount }}</span>
          </div>
        </div>
      </div>
    </div>
  </transition>
</template>

<script lang="ts">
import { defineComponent, ref, computed, watch, PropType } from 'vue';
import type { PageInfo, PageReference } from '../kernel-page-database';

export default defineComponent({
  name: 'KernelPageTooltip',

  props: {
    pageInfo: {
      type: Object as PropType<PageInfo | null>,
      default: null
    },
    x: {
      type: Number,
      default: 0
    },
    y: {
      type: Number,
      default: 0
    },
    visible: {
      type: Boolean,
      default: false
    },
    pinned: {
      type: Boolean,
      default: false
    }
  },

  emits: ['close', 'pin'],

  setup(props, { emit }) {
    const isHovered = ref(false);
    const hideTimeout = ref<number | null>(null);

    // Computed properties
    const tooltipStyle = computed(() => {
      // Position tooltip near cursor but ensure it stays on screen
      const padding = 10;
      let left = props.x + padding;
      let top = props.y + padding;

      // Estimate tooltip dimensions
      const tooltipWidth = 400;
      const tooltipHeight = 300;

      // Adjust if would go off screen
      if (left + tooltipWidth > window.innerWidth) {
        left = props.x - tooltipWidth - padding;
      }
      if (top + tooltipHeight > window.innerHeight) {
        top = props.y - tooltipHeight - padding;
      }

      // Ensure not off left or top edge
      left = Math.max(padding, left);
      top = Math.max(padding, top);

      return {
        left: `${left}px`,
        top: `${top}px`
      };
    });

    const groupedReferences = computed(() => {
      if (!props.pageInfo) return {};

      const groups: Record<number, PageReference[]> = {};
      for (const ref of props.pageInfo.references) {
        if (!groups[ref.pid]) {
          groups[ref.pid] = [];
        }
        groups[ref.pid].push(ref);
      }
      return groups;
    });

    const uniqueProcessCount = computed(() => {
      return Object.keys(groupedReferences.value).length;
    });

    const pteCount = computed(() => {
      if (!props.pageInfo) return 0;
      return props.pageInfo.references.filter(r => r.type === 'pte').length;
    });

    const sectionCount = computed(() => {
      if (!props.pageInfo) return 0;
      return props.pageInfo.references.filter(r => r.type === 'section').length;
    });

    const showSummary = computed(() => {
      return props.pageInfo && props.pageInfo.references.length > 3;
    });

    // Methods
    const formatAddress = (addr: number | bigint): string => {
      if (typeof addr === 'bigint') {
        return `0x${addr.toString(16).padStart(16, '0')}`;
      }
      return `0x${addr.toString(16).padStart(8, '0')}`;
    };

    const formatBytes = (bytes: number): string => {
      if (bytes < 1024) return `${bytes}B`;
      if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)}KB`;
      if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)}MB`;
      return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)}GB`;
    };

    const handleMouseEnter = () => {
      isHovered.value = true;
      if (hideTimeout.value) {
        clearTimeout(hideTimeout.value);
        hideTimeout.value = null;
      }
    };

    const handleMouseLeave = () => {
      isHovered.value = false;
      if (!props.pinned) {
        // Hide after a short delay when mouse leaves
        hideTimeout.value = window.setTimeout(() => {
          if (!isHovered.value) {
            emit('close');
          }
        }, 300);
      }
    };

    // Watch for visibility changes
    watch(() => props.visible, (newVal) => {
      if (!newVal && hideTimeout.value) {
        clearTimeout(hideTimeout.value);
        hideTimeout.value = null;
      }
    });

    return {
      isHovered,
      tooltipStyle,
      groupedReferences,
      uniqueProcessCount,
      pteCount,
      sectionCount,
      showSummary,
      formatAddress,
      formatBytes,
      handleMouseEnter,
      handleMouseLeave
    };
  }
});
</script>

<style scoped>
.kernel-page-tooltip {
  position: fixed;
  background: rgba(20, 20, 25, 0.98);
  border: 1px solid #4a5568;
  border-radius: 8px;
  padding: 0;
  max-width: 450px;
  max-height: 500px;
  overflow: hidden;
  box-shadow: 0 10px 40px rgba(0, 0, 0, 0.5);
  z-index: 10000;
  font-size: 12px;
  backdrop-filter: blur(10px);
  display: flex;
  flex-direction: column;
}

.tooltip-header {
  padding: 10px 12px;
  background: rgba(74, 85, 104, 0.3);
  border-bottom: 1px solid #4a5568;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.page-address {
  font-family: 'Courier New', monospace;
  font-weight: bold;
  color: #90cdf4;
  font-size: 13px;
}

.page-badges {
  display: flex;
  gap: 6px;
}

.badge {
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 10px;
  font-weight: bold;
  text-transform: uppercase;
}

.badge.kernel {
  background: rgba(239, 68, 68, 0.3);
  color: #fca5a5;
  border: 1px solid rgba(239, 68, 68, 0.5);
}

.badge.shared {
  background: rgba(59, 130, 246, 0.3);
  color: #93c5fd;
  border: 1px solid rgba(59, 130, 246, 0.5);
}

.badge.zero {
  background: rgba(107, 114, 128, 0.3);
  color: #d1d5db;
  border: 1px solid rgba(107, 114, 128, 0.5);
}

.tooltip-body {
  padding: 12px;
  overflow-y: auto;
  flex: 1;
}

.no-references {
  color: #9ca3af;
  font-style: italic;
  text-align: center;
  padding: 20px;
}

.reference-count {
  color: #d1d5db;
  font-weight: bold;
  margin-bottom: 10px;
  padding-bottom: 6px;
  border-bottom: 1px solid #374151;
}

.process-group {
  margin-bottom: 12px;
  padding: 8px;
  background: rgba(31, 41, 55, 0.5);
  border-radius: 6px;
}

.process-header {
  display: flex;
  align-items: baseline;
  gap: 6px;
  margin-bottom: 6px;
  padding-bottom: 4px;
  border-bottom: 1px solid #374151;
}

.process-name {
  font-weight: bold;
  color: #60a5fa;
  font-size: 13px;
}

.process-pid {
  color: #9ca3af;
  font-size: 11px;
}

.reference-items {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.reference-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 6px;
  background: rgba(17, 24, 39, 0.5);
  border-radius: 4px;
  font-size: 11px;
}

.ref-type {
  padding: 1px 4px;
  border-radius: 3px;
  font-weight: bold;
  font-size: 10px;
}

.ref-type.pte {
  background: rgba(34, 197, 94, 0.2);
  color: #86efac;
  border: 1px solid rgba(34, 197, 94, 0.4);
}

.ref-type.section {
  background: rgba(168, 85, 247, 0.2);
  color: #c4b5fd;
  border: 1px solid rgba(168, 85, 247, 0.4);
}

.section-type {
  color: #fbbf24;
  font-weight: 500;
  text-transform: capitalize;
}

.virtual-addr {
  font-family: 'Courier New', monospace;
  color: #e5e7eb;
  flex: 1;
}

.permissions {
  font-family: 'Courier New', monospace;
  color: #34d399;
  font-weight: bold;
}

.size {
  color: #9ca3af;
  font-size: 10px;
}

.tooltip-summary {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid #374151;
  display: flex;
  gap: 16px;
  font-size: 11px;
}

.summary-item {
  display: flex;
  gap: 6px;
  align-items: baseline;
}

.summary-item .label {
  color: #9ca3af;
}

.summary-item .value {
  font-weight: bold;
  color: #e5e7eb;
}

/* Transition animations */
.tooltip-fade-enter-active,
.tooltip-fade-leave-active {
  transition: opacity 0.2s ease, transform 0.2s ease;
}

.tooltip-fade-enter-from {
  opacity: 0;
  transform: translateY(-10px);
}

.tooltip-fade-leave-to {
  opacity: 0;
  transform: translateY(10px);
}

/* Scrollbar styling */
.tooltip-body::-webkit-scrollbar {
  width: 6px;
}

.tooltip-body::-webkit-scrollbar-track {
  background: rgba(31, 41, 55, 0.3);
  border-radius: 3px;
}

.tooltip-body::-webkit-scrollbar-thumb {
  background: #4b5563;
  border-radius: 3px;
}

.tooltip-body::-webkit-scrollbar-thumb:hover {
  background: #6b7280;
}
</style>