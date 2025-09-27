#!/usr/bin/env node

import fs from 'fs';

const GUEST_RAM_START = 0x40000000;
const TASK_STRUCT_SIZE = 9088;
const PID_OFFSET = 0x750;
const COMM_OFFSET = 0x970;

console.log('=== How the Kernel REALLY Accesses task_structs ===\n');

const memoryPath = '/tmp/haywire-vm-mem';
const fd = fs.openSync(memoryPath, 'r');

// Example: task_struct at offset 0x4700 that straddles pages
// PA pages: 0x100004000, 0x100005000, 0x100006000

console.log('The Problem: task_struct at SLAB offset 0x4700');
console.log('============================================\n');

console.log('Physical Reality:');
console.log('  Offset 0x4700 spans 3 pages:');
console.log('    - Page 4: 0x700-0xFFF (2304 bytes)');
console.log('    - Page 5: 0x000-0xFFF (4096 bytes)');
console.log('    - Page 6: 0x000-0xA7F (2688 bytes)');
console.log('');
console.log('If pages are NOT physically contiguous:');
console.log('  Page 4 at PA: 0x100004000');
console.log('  Page 5 at PA: 0x123456000 (somewhere else!)');
console.log('  Page 6 at PA: 0x198765000 (yet another place!)');
console.log('');
console.log('Reading PID field (at offset 0x4700 + 0x750 = 0x4E50):');
console.log('  Would need to read from PA 0x100004E50');
console.log('Reading comm field (at offset 0x4700 + 0x970 = 0x5070):');
console.log('  Would need to read from PA 0x123456070 (different page!)');
console.log('\n' + '='.repeat(60) + '\n');

console.log('The Solution: KERNEL VIRTUAL ADDRESSES');
console.log('=====================================\n');

console.log('The kernel NEVER accesses task_structs via physical addresses!');
console.log('');
console.log('Instead, the kernel:');
console.log('1. Has a pointer to task_struct as a VIRTUAL ADDRESS');
console.log('   Example: current->mm = 0xffff0000c1234000');
console.log('');
console.log('2. The VA points to a VIRTUALLY CONTIGUOUS mapping:');
console.log('   VA 0xffff0000c1234000-0xffff0000c1236380 (9088 bytes)');
console.log('');
console.log('3. The page tables map this to the scattered physical pages:');
console.log('   VA 0xffff0000c1234700-0xffff0000c1234FFF -> PA 0x100004700-0x100004FFF');
console.log('   VA 0xffff0000c1235000-0xffff0000c1235FFF -> PA 0x123456000-0x123456FFF');
console.log('   VA 0xffff0000c1236000-0xffff0000c1236A7F -> PA 0x198765000-0x198765A7F');
console.log('');
console.log('4. When kernel code does:');
console.log('   int pid = task->pid;  // task + 0x750');
console.log('   char *comm = task->comm;  // task + 0x970');
console.log('');
console.log('   The CPU/MMU automatically translates:');
console.log('   - VA 0xffff0000c1234750 -> PA 0x100004750 (PID)');
console.log('   - VA 0xffff0000c1234970 -> PA 0x100004970 (comm)');
console.log('   Both in the SAME virtual "view" of the structure!');
console.log('\n' + '='.repeat(60) + '\n');

console.log('Why Our Scanner Misses 9%:');
console.log('=========================\n');
console.log('We scan PHYSICAL memory directly:');
console.log('- We assume offset 0x4700 continues at PA + 0x2380');
console.log('- But if SLUB got non-contiguous pages, it\'s not there!');
console.log('- The kernel doesn\'t care - it uses VAs');
console.log('');
console.log('The 9% missing processes have task_structs where:');
console.log('- SLUB allocated from non-contiguous physical pages');
console.log('- Our scanner can\'t follow the discontinuity');
console.log('- But kernel accesses them fine via virtual addresses');
console.log('\n' + '='.repeat(60) + '\n');

console.log('How /proc Gets 100%:');
console.log('===================\n');
console.log('1. /proc walks init_pid_ns.idr (we found this!)');
console.log('2. Each PID maps to a struct pid pointer (VA)');
console.log('3. struct pid contains task pointer (also VA)');
console.log('4. Kernel dereferences: task->pid, task->comm');
console.log('5. MMU handles VA->PA translation seamlessly');
console.log('6. Never needs to know about physical fragmentation!');
console.log('');
console.log('Example from our findings:');
console.log('  PID 1 (systemd): task_struct at VA 0xffff000003388000');
console.log('  Kernel just does: systemd->pid, systemd->comm');
console.log('  MMU translates to whatever physical pages needed');
console.log('\n' + '='.repeat(60) + '\n');

console.log('The Fundamental Issue:');
console.log('=====================\n');
console.log('Physical Memory View (what we scan):');
console.log('  [PA 0x100000000] [TASK1_PART1] [OTHER_DATA] [GAP]');
console.log('  [PA 0x123456000] [RANDOM] [TASK1_PART2] [STUFF]');
console.log('  [PA 0x198765000] [TASK1_PART3] [MORE_RANDOM]');
console.log('');
console.log('Virtual Memory View (what kernel uses):');
console.log('  [VA 0xffff0000...] [TASK1_COMPLETE_9088_BYTES]');
console.log('  Clean, contiguous, no gaps!');
console.log('');
console.log('To get 100%, we need to either:');
console.log('1. Parse kernel data structures (IDR) that use VAs');
console.log('2. Understand SLUB metadata to find all fragments');
console.log('3. Build a VA->PA mapping and scan virtual space');
console.log('');
console.log('The kernel NEVER deals with fragmented task_structs');
console.log('because it NEVER sees them - only contiguous VAs!');

fs.closeSync(fd);