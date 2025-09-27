#!/bin/bash

# Fix arithmetic operations in kernel-discovery-paged.ts
FILE="src/kernel-discovery-paged.ts"

# Line 713: addr: offset + KernelConstants.GUEST_RAM_START
sed -i '' '713s/offset + KernelConstants.GUEST_RAM_START/PA(offset + Number(KernelConstants.GUEST_RAM_START))/' $FILE

# Line 1023: offset + KernelConstants.GUEST_RAM_START
sed -i '' '1023s/offset + KernelConstants.GUEST_RAM_START/offset + Number(KernelConstants.GUEST_RAM_START)/' $FILE

# Lines with subtraction patterns: X - KernelConstants.GUEST_RAM_START
sed -i '' 's/\([a-zA-Z0-9_]*\) - KernelConstants.GUEST_RAM_START/\1 - Number(KernelConstants.GUEST_RAM_START)/g' $FILE

# Lines with subtraction patterns: X - this.swapperPgDir
sed -i '' 's/\([a-zA-Z0-9_]*\) - this.swapperPgDir/\1 - Number(this.swapperPgDir)/g' $FILE

# Lines with addition: this.swapperPgDir + X
sed -i '' 's/this.swapperPgDir + \([0-9]*\)/PhysicalAddress.add(this.swapperPgDir, \1)/g' $FILE

echo "Fixed arithmetic operations in $FILE"