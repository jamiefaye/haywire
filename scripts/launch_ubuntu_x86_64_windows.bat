@echo off
REM Launch x86_64 Ubuntu on Windows with WHPX acceleration
REM WARNING: WHPX has known issues with Linux guests (MSI injection, MMIO errors)
REM Recommended: Use WSL2 + KVM instead (see docs/windows_deployment.md)

set UBUNTU_ISO=%USERPROFILE%\haywire\vms\ubuntu-24.04-desktop-amd64.iso
set DISK_IMAGE=%USERPROFILE%\haywire\vms\ubuntu_x86_64.qcow2
set QEMU_PATH=C:\Program Files\qemu\qemu-system-x86_64.exe

set MEMORY=4G
set CORES=4
set QMP_PORT=4445
set MONITOR_PORT=4444

echo ===============================================
echo x86_64 Ubuntu with WHPX Acceleration (Windows)
echo ===============================================
echo.
echo WARNING: WHPX has stability issues with Linux guests
echo Recommended: Use WSL2 + KVM instead
echo.

REM Check if QEMU is installed
if not exist "%QEMU_PATH%" (
    echo ERROR: QEMU not found at: %QEMU_PATH%
    echo.
    echo Please install QEMU for Windows from:
    echo https://qemu.weilnetz.de/w64/
    echo.
    pause
    exit /b 1
)

REM Create vms directory if needed
if not exist "%USERPROFILE%\haywire\vms" mkdir "%USERPROFILE%\haywire\vms"

REM Create disk if needed
if not exist "%DISK_IMAGE%" (
    echo Creating disk image: %DISK_IMAGE% ^(40G^)
    "%QEMU_PATH:qemu-system-x86_64.exe=qemu-img.exe%" create -f qcow2 "%DISK_IMAGE%" 40G
    set FIRST_BOOT=1
)

REM Check if this is first boot
if defined FIRST_BOOT (
    echo.
    echo === FIRST BOOT DETECTED ===
    echo Will boot from ISO to install Ubuntu
    echo.
    if not exist "%UBUNTU_ISO%" (
        echo ERROR: Ubuntu ISO not found at: %UBUNTU_ISO%
        echo.
        echo Download from:
        echo Desktop: https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso
        echo Server:  https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso
        echo.
        echo Save to: %UBUNTU_ISO%
        pause
        exit /b 1
    )
    set BOOT_ORDER=-boot d
    set CDROM_OPTION=-cdrom "%UBUNTU_ISO%"
) else (
    echo Booting from existing installation...
    set BOOT_ORDER=-boot c
    set CDROM_OPTION=
)

echo Starting QEMU with WHPX acceleration...
echo Ports: QMP=%QMP_PORT%, Monitor=%MONITOR_PORT%
echo.

REM Note: memory-backend-file does NOT work on Windows
REM Path would need to be \\.\pipe\haywire-vm-mem or similar
"%QEMU_PATH%" ^
    -machine q35,accel=whpx,kernel-irqchip=off ^
    -cpu host ^
    -m %MEMORY% ^
    -object memory-backend-file,id=mem,size=%MEMORY%,mem-path=\\.\pipe\haywire-vm-mem,share=on,prealloc=off ^
    -numa node,memdev=mem ^
    -smp %CORES% ^
    -device VGA ^
    -display sdl ^
    -device qemu-xhci ^
    -device usb-kbd ^
    -device usb-tablet ^
    -device intel-hda ^
    -device hda-duplex ^
    -drive if=virtio,format=qcow2,file="%DISK_IMAGE%" ^
    %CDROM_OPTION% ^
    %BOOT_ORDER% ^
    -qmp tcp:localhost:%QMP_PORT%,server,nowait ^
    -monitor telnet:localhost:%MONITOR_PORT%,server,nowait ^
    -chardev socket,path=\\.\pipe\qga,server=on,wait=off,id=qga0 ^
    -device virtio-serial ^
    -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 ^
    -netdev user,id=net0,hostfwd=tcp::2222-:22 ^
    -device virtio-net-pci,netdev=net0 ^
    -serial stdio ^
    -name "Ubuntu-x86_64-WHPX"

echo.
echo VM shut down.

if defined FIRST_BOOT (
    echo.
    echo === INSTALLATION COMPLETE ===
    echo Run this script again to boot from the installed system
)

pause
