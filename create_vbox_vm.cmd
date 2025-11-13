@echo off
REM Create VirtualBox VM for Ubuntu x86_64

set VM_NAME=Ubuntu-x86_64-Haywire
set VDI_PATH=C:\haywire-vbox\ubuntu_x86_64.vdi
set VBOX=C:\Program Files\Oracle\VirtualBox\VBoxManage.exe

echo Creating VirtualBox VM: %VM_NAME%
echo.

REM Create VM
"%VBOX%" createvm --name "%VM_NAME%" --ostype Ubuntu_64 --register

REM Configure VM
"%VBOX%" modifyvm "%VM_NAME%" ^
    --memory 4096 ^
    --cpus 4 ^
    --vram 128 ^
    --boot1 disk ^
    --nic1 nat ^
    --natpf1 "ssh,tcp,,2222,,22" ^
    --audio none

REM Create SATA controller and attach disk
"%VBOX%" storagectl "%VM_NAME%" --name "SATA" --add sata --controller IntelAhci
"%VBOX%" storageattach "%VM_NAME%" ^
    --storagectl "SATA" ^
    --port 0 ^
    --device 0 ^
    --type hdd ^
    --medium "%VDI_PATH%"

REM Enable debugging (REQUIRED for memory dumps)
"%VBOX%" modifyvm "%VM_NAME%" --debug on

echo.
echo ===============================================
echo VM created successfully!
echo.
echo Name: %VM_NAME%
echo Memory: 4GB
echo CPUs: 4
echo Disk: %VDI_PATH%
echo SSH Port: 2222 (forwarded from guest port 22)
echo Debugging: ENABLED
echo.
echo Next steps:
echo 1. Start VM:  VBoxManage startvm "%VM_NAME%" --type headless
echo 2. Dump RAM:  scripts\vbox_dump_memory.cmd "%VM_NAME%"
echo ===============================================
pause
