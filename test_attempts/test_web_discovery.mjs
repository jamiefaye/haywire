#!/usr/bin/env node

import fs from 'fs';
import { PagedMemoryReader } from './web/dist/paged-memory.js';
import { KernelDiscoveryPaged } from './web/dist/kernel-discovery-paged.js';

const memPath = '/tmp/haywire-vm-mem';
const fileSize = fs.statSync(memPath).size;

console.log('Running web version kernel discovery...');
console.log(`Memory file: ${memPath} (${fileSize / (1024*1024*1024)}GB)`);

// Create memory reader
const memory = new PagedMemoryReader(memPath, fileSize);

// Create kernel discovery
const discovery = new KernelDiscoveryPaged(memory, fileSize);

// Discover processes
const processes = discovery.discoverProcesses();

console.log(`\n=== WEB VERSION RESULTS ===`);
console.log(`Found ${processes.size} processes`);

// Count unique names
const uniqueNames = new Set();
for (const [pid, proc] of processes) {
    uniqueNames.add(proc.name);
}
console.log(`Unique names: ${uniqueNames.size}`);

// Show first 10 processes
console.log('\nFirst 10 processes:');
let count = 0;
for (const [pid, proc] of processes) {
    console.log(`  PID ${pid}: ${proc.name}`);
    count++;
    if (count >= 10) break;
}