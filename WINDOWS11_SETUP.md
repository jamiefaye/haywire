# Windows 11 QEMU Setup for Haywire

This guide walks you through setting up a Windows 11 VM in QEMU with memory-backend-file support for Haywire introspection.

## Prerequisites

### 1. Install Required Packages (WSL Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y qemu-system-x86 ovmf swtpm swtpm-tools
```

### 2. Download Windows 11 ISO

**Option A: Official Microsoft Download**
1. Visit: https://www.microsoft.com/software-download/windows11
2. Click "Download Windows 11 Disk Image (ISO)"
3. Select "Windows 11 (multi-edition ISO)"
4. Choose language and download (64-bit)

**Option B: Windows Insider Preview (for testing)**
1. Visit: https://www.microsoft.com/en-us/software-download/windowsinsiderpreviewiso
2. Sign in with Microsoft account
3. Download latest Windows 11 Preview ISO

Save the ISO to: `~/haywire/isos/Win11_24H2_English_x64.iso`

```bash
mkdir -p ~/haywire/isos
# Move your downloaded ISO here
```

### 3. (Optional) Download VirtIO Drivers

For better performance, download the VirtIO drivers:

```bash
cd ~/haywire/isos
wget https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso
```

## Installation

### 1. Make the launch script executable

```bash
cd ~/haywire/scripts
chmod +x launch_windows11.sh
```

### 2. Start the VM

```bash
./launch_windows11.sh
```

### 3. Install Windows 11

The VM will boot from the ISO. Follow the Windows installation:

#### Bypassing TPM/Secure Boot Requirements (if needed)

If Windows complains about TPM or system requirements:
1. Press **Shift+F10** to open Command Prompt
2. Type: `regedit`
3. Navigate to: `HKEY_LOCAL_MACHINE\SYSTEM\Setup`
4. Create new key: `LabConfig`
5. Inside LabConfig, create these DWORD values:
   - `BypassTPMCheck` = 1
   - `BypassSecureBootCheck` = 1
   - `BypassRAMCheck` = 1
6. Close regedit and continue installation

#### Bypassing Microsoft Account Requirement

During setup when asked to sign in:
1. Press **Shift+F10** to open Command Prompt
2. Type: `OOBE\BYPASSNRO`
3. Press Enter (system will reboot)
4. After reboot, you'll see "I don't have internet" option
5. Click it to create a local account

#### Storage Driver (if disk not detected)

If Windows installer doesn't see the disk:
1. Click "Load driver"
2. Browse to the VirtIO ISO (if you added it)
3. Select `vioscsi\w11\amd64` folder
4. Install the driver
5. The QEMU disk should now appear

### 4. Post-Installation

After Windows is installed:

1. **Install VirtIO drivers** (if you have the ISO):
   - Run `virtio-win-guest-tools.exe` from the ISO
   - This installs network, storage, and display drivers

2. **Disable Windows Defender** (optional, for better performance):
   - Settings → Privacy & Security → Windows Security
   - Virus & threat protection → Manage settings
   - Turn off Real-time protection

3. **Disable Windows Update** (optional):
   - Services → Windows Update → Stop and Disable

4. **Take a snapshot** (recommended):
   ```bash
   # From WSL, after shutting down VM
   cd ~/haywire/vms
   qemu-img snapshot -c "fresh-install" windows11.qcow2
   ```

## Running the VM

After installation, boot the VM with:

```bash
cd ~/haywire/scripts
./launch_windows11.sh
```

The memory will be exposed at `/tmp/haywire-vm-mem` for Haywire to access.

## Connecting with Haywire

Once Windows is running, you can use Haywire to introspect it:

```bash
cd ~/haywire
./build-linux/haywire
```

**Note**: Windows support in Haywire is not yet implemented. You'll need to:
1. Implement `WindowsKernelDiscovery` class in `src/windows/windows_kernel_discovery.cpp`
2. Add Windows-specific process discovery (EPROCESS scanning)
3. Create Windows kernel profile with offsets

## Troubleshooting

### VM won't start - KVM not available
If you see "KVM not available", this is normal in WSL. QEMU will use TCG (slower but works).

### TPM error during Windows install
Make sure `swtpm` is installed:
```bash
sudo apt-get install swtpm swtpm-tools
```

### Display issues
Try adding `-vga virtio` to the QEMU command in the script for better graphics.

### Slow performance
- Increase RAM: Change `MEMORY="8G"` to `MEMORY="12G"` in the script
- Increase CPUs: Change `CORES="4"` to `CORES="8"`
- Install VirtIO drivers for better I/O performance

### Memory file not created
Check permissions on `/tmp`:
```bash
ls -ld /tmp
# Should show: drwxrwxrwt
```

## Next Steps

1. **Implement Windows Kernel Discovery**:
   - Study EPROCESS structure offsets for Windows 11
   - Implement process scanning in `src/windows/windows_kernel_discovery.cpp`
   - Create kernel profile in `profiles/windows/`

2. **Test with Haywire**:
   - Verify memory-backend-file is accessible
   - Test EPROCESS scanning
   - Validate process discovery

3. **Create Windows kernel profile**:
   - Use WinDbg or similar tools to get structure offsets
   - Document in `profiles/windows/win11-22H2-x64.json`

## Reference

- Windows kernel structures: https://www.vergiliusproject.com/
- EPROCESS documentation: https://www.nirsoft.net/kernel_struct/vista/EPROCESS.html
- VirtIO drivers: https://github.com/virtio-win/virtio-win-pkg-scripts
