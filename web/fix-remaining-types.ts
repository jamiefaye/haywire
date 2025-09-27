#!/usr/bin/env node
/**
 * Script to help identify and fix remaining type issues
 */

import * as fs from 'fs';
import * as path from 'path';

// Common fix patterns
const fixes = [
  // Fix arithmetic operations
  {
    pattern: /(\w+) - (KernelConstants\.(GUEST_RAM_START|GUEST_RAM_END))/g,
    replacement: '$1 - Number($2)'
  },
  {
    pattern: /(\w+) \+ (KernelConstants\.(GUEST_RAM_START|GUEST_RAM_END))/g,
    replacement: '$1 + Number($2)'
  },
  // Fix comparisons
  {
    pattern: /(\w+) > (KernelConstants\.(GUEST_RAM_START|GUEST_RAM_END))/g,
    replacement: 'Number($1) > Number($2)'
  },
  {
    pattern: /(\w+) < (KernelConstants\.(GUEST_RAM_START|GUEST_RAM_END))/g,
    replacement: 'Number($1) < Number($2)'
  },
  // Fix PA/VA creation
  {
    pattern: /: (0x[0-9a-fA-F]+),(\s*\/\/ Physical)/g,
    replacement: ': PA($1),$2'
  },
  {
    pattern: /: (0xffff[0-9a-fA-F]+n),(\s*\/\/ Virtual)/g,
    replacement: ': VA($1),$2'
  }
];

function processFile(filePath: string) {
  let content = fs.readFileSync(filePath, 'utf-8');
  let modified = false;

  fixes.forEach(fix => {
    const newContent = content.replace(fix.pattern, fix.replacement);
    if (newContent !== content) {
      modified = true;
      content = newContent;
    }
  });

  if (modified) {
    console.log(`Fixed ${filePath}`);
    // Uncomment to actually write:
    // fs.writeFileSync(filePath, content, 'utf-8');
  }
}

// Process TypeScript files
const srcDir = path.join(__dirname, 'src');
const files = [
  'kernel-discovery.ts',
  'kernel-discovery-paged.ts',
  'kernel-mem.ts',
  'kernel-page-database.ts'
];

files.forEach(file => {
  const filePath = path.join(srcDir, file);
  if (fs.existsSync(filePath)) {
    processFile(filePath);
  }
});

console.log('\nRemaining manual fixes needed:');
console.log('1. Update Map<number, ...> to Map<string, ...> for physical addresses');
console.log('2. Change pageToPids.has(pa) to pageToPids.has(PhysicalAddress.toHex(pa))');
console.log('3. Update return types in interfaces');
console.log('4. Fix stripPAC calls to use VA() wrapper');
console.log('5. Update section creation to use VA() and PA()');