@echo off
REM Launch x86_64 Ubuntu on Intel Windows with WHPX acceleration
REM This uses Windows Hypervisor Platform for NATIVE performance (not emulation!)

set UBUNTU_ISO=C:\haywire\ubuntu-24.04-desktop-amd64.iso
set DISK_IMAGE=..\vms\ubuntu_x86_64.qcow2
set QEMU_PATH=C:\Program Files\qemu

REM QEMU configuration
set MEMORY=4G
set CORES=4
set QMP_PORT=4445
set MONITOR_PORT=4444

echo === x86_64 Ubuntu with WHPX Acceleration ===
echo This should be FAST on Intel Windows - using native CPU!
echo.

REM Check if disk exists, create if needed
if not exist "%DISK_IMAGE%" (
    echo Creating disk image: %DISK_IMAGE% (20G)
    "%QEMU_PATH%\qemu-img.exe" create -f qcow2 "%DISK_IMAGE%" 20G
)

REM Check if ISO exists
if not exist "%UBUNTU_ISO%" (
    echo x86_64 Ubuntu ISO not found!
    echo.
    echo Download from:
    echo Desktop: https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso
    echo Server: https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso
    echo.
    echo Save as: %UBUNTU_ISO%
    pause
    exit /b 1
)

echo Starting QEMU with WHPX acceleration (native Intel)...
echo Ports: QMP=%QMP_PORT%, Monitor=%MONITOR_PORT%

REM Find UEFI firmware for x86_64
set UEFI_CODE=%QEMU_PATH%\share\edk2-x86_64-code.fd
if not exist "%UEFI_CODE%" (
    echo WARNING: UEFI firmware not found, using BIOS mode
    set UEFI_CODE=
)

REM Launch with WHPX acceleration (Windows Hypervisor Platform)
REM -cpu host = use ALL host CPU features (fastest!)
REM
REM IMPORTANT: Windows Hypervisor Platform must be enabled:
REM   1. Control Panel -> Programs -> Turn Windows features on or off
REM   2. Check "Windows Hypervisor Platform"
REM   3. Reboot

"%QEMU_PATH%\qemu-system-x86_64.exe" ^
    -M q35 ^
    -accel whpx ^
    -cpu host ^
    -m %MEMORY% ^
    -smp %CORES% ^
    %UEFI_CODE:+-bios "%UEFI_CODE%"% ^
    -device virtio-vga ^
    -display default,show-cursor=on ^
    -device qemu-xhci ^
    -device usb-kbd ^
    -device usb-tablet ^
    -device intel-hda ^
    -device hda-duplex ^
    -drive if=virtio,format=qcow2,file="%DISK_IMAGE%" ^
    -cdrom "%UBUNTU_ISO%" ^
    -boot d ^
    -object memory-backend-file,id=mem,size=%MEMORY%,mem-path=\\.\pipe\haywire-vm-mem,share=on ^
    -numa node,memdev=mem ^
    -qmp tcp:localhost:%QMP_PORT%,server,nowait ^
    -monitor telnet:localhost:%MONITOR_PORT%,server,nowait ^
    -chardev socket,path=\\.\pipe\qga,server=on,wait=off,id=qga0 ^
    -device virtio-serial ^
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 ^
    -netdev user,id=net0,hostfwd=tcp::2222-:22 ^
    -device virtio-net-pci,netdev=net0 ^
    -serial stdio ^
    -name "Ubuntu-x86_64-WHPX"

echo VM shut down.
pause
