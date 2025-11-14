# Run VirtualBox COM API Test - Windows Native

## Quick Start (No WSL!)

### Step 1: Open Windows Terminal or PowerShell

Just open a regular Windows command prompt or PowerShell.

### Step 2: Navigate to Haywire directory

```cmd
cd C:\Users\jamie\haywire
```

### Step 3: Run the test batch file

```cmd
build_and_test_vbox_com.cmd
```

That's it! The script will:
1. Find Visual Studio
2. Compile the test program
3. Show available VMs
4. Ask which VM to test
5. Start the VM if needed
6. Run the COM API test

---

## Alternative: Manual Steps

If the batch file has issues, here's the manual approach:

### 1. Open "Developer Command Prompt for VS 2022"

Search Windows Start menu for: **"Developer Command Prompt for VS 2022"**

(This sets up the compiler environment)

### 2. Navigate to haywire

```cmd
cd C:\Users\jamie\haywire
```

### 3. Check if VirtualBox SDK exists

```cmd
dir "C:\Program Files\Oracle\VirtualBox\sdk"
```

**If SDK not found:** Download it from https://www.virtualbox.org/wiki/Downloads
- Look for "VirtualBox SDK"
- Extract to `C:\Program Files\Oracle\VirtualBox\sdk`

### 4. Compile the test program

```cmd
cl /EHsc test_vbox_com.cpp ole32.lib oleaut32.lib
```

Should create `test_vbox_com.exe`

### 5. Check if VM is running

```cmd
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" list runningvms
```

### 6. Start VM if needed

```cmd
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm "Ubuntu-x86_64-Haywire" --type headless
```

Wait ~15 seconds for boot.

### 7. Run the test!

```cmd
test_vbox_com.exe "Ubuntu-x86_64-Haywire" 0x0 4096
```

**Note:** Address `0x0` is for x86_64. For ARM64, use `0x40000000`

---

## What to Look For

### ✅ SUCCESS Output:

```
✅ SUCCESS! ReadPhysicalMemory IS IMPLEMENTED!

Read 4096 bytes from 0x00000000

First 64 bytes:
  00000000:  eb 48 90 00 00 00 00 00  ...
  00000010:  00 00 00 00 00 00 00 00  ...

🎉 VirtualBox memory bridge is FEASIBLE!
   We can use COM API without recompiling VirtualBox.
```

**This means:** We can build the memory bridge! No VirtualBox compilation needed!

### ❌ FAILURE Output:

```
❌ FAILED! ReadPhysicalMemory NOT implemented

Error code: 0x80004001

The method exists but returns NotImplemented.
This is VirtualBox ticket #10222.
```

**This means:** We need to use alternative approaches (dumps or patch).

---

## Troubleshooting

### "cl is not recognized"

You need to run from **Developer Command Prompt for VS 2022**, not regular Command Prompt.

Or manually run:
```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

### "VirtualBox SDK not found"

Download SDK from: https://www.virtualbox.org/wiki/Downloads

Extract the `sdk` folder to: `C:\Program Files\Oracle\VirtualBox\sdk`

### "VM not found"

List VMs:
```cmd
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" list vms
```

Use exact name from output (with quotes if it has spaces).

### "VM is not running"

Start it:
```cmd
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm "YOUR-VM-NAME" --type headless
```

---

## Next Steps Based on Results

### If Test Succeeds ✅

1. I'll create a `VBoxConnection` class for Haywire
2. Add it alongside your existing `QemuConnection`
3. Haywire can directly access VirtualBox guest memory
4. No separate bridge daemon needed!

### If Test Fails ❌

Choose one:
1. **Keep using QEMU** (already works perfectly with memory-backend-file)
2. **Use VirtualBox dumps** (your existing scripts work)
3. **Patch VirtualBox** (best performance, but requires compilation)

---

## Ready to Go!

Just run:
```cmd
cd C:\Users\jamie\haywire
build_and_test_vbox_com.cmd
```

Or use the manual steps above if the batch file doesn't work.

**Let me know what the test says!** 🚀
