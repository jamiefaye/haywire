# Haywire Quick Start Guide - Windows with WSL2

This guide walks you through starting the Ubuntu VM and running Haywire for memory introspection.

## Prerequisites

- Windows 10/11 with WSL2 installed
- Ubuntu in WSL2 with QEMU built (modified version in `qemu-mods/`)
- VNC client installed on Windows (TigerVNC, RealVNC, or TightVNC)
- Haywire built (executable at `build/haywire`)

## Terminal Legend

Throughout this guide:
- **[WSL]** = Run in WSL2 terminal (Ubuntu bash)
- **[Windows]** = Run in Windows terminal (PowerShell or CMD)

---

## Step 1: Launch the Ubuntu VM

### 1.1 Open WSL2 Terminal

**[Windows]** Open PowerShell or Windows Terminal and start WSL:

```powershell
wsl
```

You should now see a Linux prompt like: `jamie@COMPUTERNAME:~$`

### 1.2 Navigate to Haywire Directory

**[WSL]** Change to the haywire directory:

```bash
cd ~/haywire
```

### 1.3 Launch the VM

**[WSL]** Run the launch script:

```bash
bash scripts/launch_ubuntu_x86_64_linux.sh
```

You should see output like:
```
=== x86_64 Ubuntu with KVM Acceleration ===
Optimized for WSL2 + nested virtualization

Booting from existing installation...
Starting QEMU with KVM acceleration...
Ports: QMP=4445, Monitor=4444

Using modified QEMU: /home/jamie/haywire/qemu-mods/qemu-src/build/qemu-system-x86_64
```

**Important:** This terminal will now be occupied by the VM. Keep it open!

### 1.4 Verify VM is Running

**[Windows]** Open a NEW PowerShell window and check:

```powershell
wsl bash -c "ss -tln | grep -E '(4445|4444|5900)'"
```

You should see:
```
LISTEN 0      1             0.0.0.0:5900      0.0.0.0:*     <- VNC
LISTEN 0      1           127.0.0.1:4444      0.0.0.0:*     <- Monitor
LISTEN 0      1           127.0.0.1:4445      0.0.0.0:*     <- QMP
```

---

## Step 2: Connect to VM via VNC

### 2.1 Get WSL2 IP Address

**[Windows]** Run the helper script to get the WSL2 IP:

```powershell
cd C:\Users\jamie\haywire
.\scripts\get-wsl2-ip.ps1
```

This will display something like:
```
WSL2 IP Address: 172.21.71.150
Connect VNC to: 172.21.71.150:5900
```

**Note:** WSL2 uses a virtual network, so `localhost` won't work directly. You need the WSL2 IP address.

### 2.2 Launch VNC Client

**[Windows]** Open your VNC client (e.g., TigerVNC Viewer, RealVNC)

### 2.3 Connect to VM

In the VNC client, connect using the IP from step 2.1:
```
172.21.71.150:5900
```

**Replace** `172.21.71.150` with your actual WSL2 IP address.

You should now see the Ubuntu desktop in the VNC window.

### 2.4 (Optional) Set Up Localhost Forwarding

If you prefer to use `localhost:5900` instead of the IP address:

**[Windows]** Right-click PowerShell and "Run as Administrator", then:

```powershell
cd C:\Users\jamie\haywire
.\scripts\setup-wsl2-vnc.ps1
```

This sets up port forwarding so you can connect to `localhost:5900`.

**Note:** You'll need to re-run this script if:
- You restart Windows
- WSL2 IP address changes
- You see "connection refused" errors

### 2.3 Login to Ubuntu (if needed)

Use your VM credentials to log in to the Ubuntu desktop.

---

## Step 3: Verify Memory Backend File

Before running Haywire, verify the memory-backend-file exists:

**[Windows]** Check if the memory file is accessible:

```powershell
wsl bash -c "ls -lh /tmp/haywire-vm-mem"
```

You should see:
```
-rw------- 1 jamie jamie 4.0G Oct 30 12:15 /tmp/haywire-vm-mem
```

The file size should match your VM's RAM size (4GB in this case).

---

## Step 4: Build Haywire (if needed)

If you haven't built Haywire yet or need to rebuild:

### 4.1 Navigate to Haywire Directory

**[Windows]** In PowerShell:

```powershell
cd C:\Users\jamie\haywire
```

### 4.2 Create Build Directory

**[Windows]** (Only needed first time):

```powershell
mkdir build -ErrorAction SilentlyContinue
cd build
```

### 4.3 Configure with CMake

**[Windows]**:

```powershell
cmake ..
```

### 4.4 Build Haywire

**[Windows]**:

```powershell
cmake --build . --config Release
```

This creates `build/haywire` or `build/haywire.exe`

---

## Step 5: Run Haywire

### 5.1 Launch Haywire

**[Windows]** From the haywire directory:

```powershell
.\build\haywire
```

Or if you're in the build directory:

```powershell
.\haywire
```

### 5.2 Configure Memory Source

When Haywire starts, you'll see the main window. Configure it to access the VM memory:

1. **Memory File Path**: The application needs to access `/tmp/haywire-vm-mem` from WSL2
   - WSL2 path: `\\wsl$\Ubuntu\tmp\haywire-vm-mem`
   - Or use: `\\wsl.localhost\Ubuntu\tmp\haywire-vm-mem`

2. **QMP Connection**:
   - Host: `localhost` (or `127.0.0.1`)
   - Port: `4445`

### 5.3 Access WSL Files from Windows

**Important:** WSL2 files are accessible from Windows via special network paths:

```
\\wsl$\Ubuntu\tmp\haywire-vm-mem
```

Or on newer Windows versions:

```
\\wsl.localhost\Ubuntu\tmp\haywire-vm-mem
```

You can verify this works:

**[Windows]** In PowerShell:

```powershell
ls \\wsl$\Ubuntu\tmp\haywire-vm-mem
```

---

## Step 6: Start Kernel Discovery

### 6.1 Click "Discover" Button

In the Haywire UI:
1. Click the **"Discover"** or **"Select"** button
2. Haywire will scan for kernel structures and processes
3. You should see a list of discovered processes

### 6.2 Select a Process

1. Choose a process from the PID selector
2. Haywire will enable VA (Virtual Address) mode
3. You can now inspect that process's virtual memory space

---

## Troubleshooting

### VM Won't Start - Port Already in Use

**[Windows/WSL]** Kill existing QEMU processes:

```bash
wsl bash -c "killall qemu-system-x86_64"
```

Then restart the launch script.

### VNC Connection Refused

Make sure the VM launch script has VNC bound to all interfaces:
- Check line 124 in `scripts/launch_ubuntu_x86_64_linux.sh`
- It should say: `-vnc 0.0.0.0:0` (not `-vnc :0`)

### Can't Access Memory File from Haywire

The Windows path to WSL files changed in recent updates. Try both:
- `\\wsl$\Ubuntu\tmp\haywire-vm-mem` (older)
- `\\wsl.localhost\Ubuntu\tmp\haywire-vm-mem` (newer)

You can also check which one works:

**[Windows]** PowerShell:

```powershell
Test-Path "\\wsl$\Ubuntu\tmp\haywire-vm-mem"
Test-Path "\\wsl.localhost\Ubuntu\tmp\haywire-vm-mem"
```

### KVM Not Available

If you see "WARNING: /dev/kvm not accessible", you need to:

**[WSL]**:

```bash
sudo usermod -a -G kvm $USER
```

Then close WSL completely and restart it:

**[Windows]**:

```powershell
wsl --shutdown
wsl
```

---

## Quick Reference Commands

### Start VM
```bash
[WSL] cd ~/haywire && bash scripts/launch_ubuntu_x86_64_linux.sh
```

### Stop VM
```bash
[WSL] killall qemu-system-x86_64
```

### Build Haywire
```powershell
[Windows] cd C:\Users\jamie\haywire
[Windows] cmake --build build --config Release
```

### Run Haywire
```powershell
[Windows] cd C:\Users\jamie\haywire
[Windows] .\build\haywire
```

### Check VM Status
```powershell
[Windows] wsl bash -c "ss -tln | grep -E '(4445|4444|5900)'"
```

### Access Memory File
```
[Windows Path] \\wsl$\Ubuntu\tmp\haywire-vm-mem
```

---

## What's Next?

Once Haywire is running and connected to the VM:

1. **Explore Memory Modes**:
   - PA Mode: Physical address view of all RAM
   - VA Mode: Virtual address view of specific process

2. **Use Mini Bitmap Viewers**:
   - Right-click memory to spawn floating viewers
   - Good for tracking specific memory regions

3. **Try Different Pixel Formats**:
   - RGB888, RGBA8888, BGR for graphics memory
   - ARM64 Insn for viewing code disassembly
   - Hex Pixel for detailed byte inspection

4. **Monitor Memory Changes**:
   - Heat map shows real-time memory modifications
   - Green = just changed, blue = stable

5. **Search for Patterns**:
   - Use search functionality to find byte patterns
   - Useful for locating specific data structures

---

## Advanced: Running from Different Terminals

You can keep the workflow more organized:

**Terminal 1 (WSL) - VM**
```bash
cd ~/haywire
bash scripts/launch_ubuntu_x86_64_linux.sh
# Keep this open - VM running here
```

**Terminal 2 (Windows) - Haywire**
```powershell
cd C:\Users\jamie\haywire
.\build\haywire
# Haywire GUI appears
```

**Terminal 3 (Windows) - Monitoring/Debug**
```powershell
# Check VM status
wsl bash -c "ss -tln | grep 5900"

# Watch QMP commands (if debugging)
wsl bash -c "telnet localhost 4444"
```

This keeps everything organized and easy to restart if needed!
