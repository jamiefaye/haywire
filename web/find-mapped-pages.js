const fs = require('fs');

// Open the memory file
const fd = fs.openSync('/tmp/haywire-vm-mem', 'r');
const GUEST_RAM_START = 0x40000000;

// Some addresses we know have mappings from the discovery log
const testAddresses = [
  { pa: 0x888b0000, desc: "Shared by Xwayland and mutter" },
  { pa: 0x88d6f000, desc: "Shared page" },
  { pa: 0x87c71000, desc: "Shared page" },
  { pa: 0x3eff0000, desc: "Low memory kernel page" },
  { pa: 0x42cfc000, desc: "PID 1704 Xwayland PGD" },
  { pa: 0x1009d1000, desc: "PID 2337 update-notifier PGD" }
];

console.log("WHERE TO NAVIGATE IN THE MEMORY VIEWER:");
console.log("=========================================\n");

for (const {pa, desc} of testAddresses) {
  // Calculate the file offset
  let fileOffset;
  if (pa >= GUEST_RAM_START) {
    fileOffset = pa - GUEST_RAM_START;
  } else {
    fileOffset = pa;
  }
  
  // Read some data to verify
  const buffer = Buffer.alloc(16);
  try {
    fs.readSync(fd, buffer, 0, 16, fileOffset);
    const isZero = buffer.every(b => b === 0);
    
    console.log(`${desc}:`);
    console.log(`  Physical Address: 0x${pa.toString(16)}`);
    console.log(`  → Navigate to: 0x${fileOffset.toString(16)}`);
    console.log(`  Data preview: ${buffer.toString('hex').substring(0, 32)}...`);
    console.log(`  Has data: ${!isZero}\n`);
  } catch (e) {
    console.log(`  ERROR: Cannot read at offset 0x${fileOffset.toString(16)}\n`);
  }
}

fs.closeSync(fd);

console.log("\nTO SEE TOOLTIPS:");
console.log("1. In the Electron app, enter one of the 'Navigate to' addresses in the offset field");
console.log("2. For example, type: 0x488b0000");
console.log("3. Press Enter to jump there");
console.log("4. Hover over the memory to see tooltips");
