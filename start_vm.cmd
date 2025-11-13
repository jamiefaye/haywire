@echo off
set VM_NAME=Ubuntu-x86_64-Haywire
set VBOX=C:\Program Files\Oracle\VirtualBox\VBoxManage.exe

echo Starting VM: %VM_NAME%
echo This will run in headless mode (no GUI window)
echo.

"%VBOX%" startvm "%VM_NAME%" --type headless

if errorlevel 1 (
    echo.
    echo ERROR: Failed to start VM!
    pause
    exit /b 1
)

echo.
echo VM started successfully!
echo.
echo The VM is now running in the background.
echo You can connect via SSH: ssh -p 2222 ubuntu@localhost
echo.
echo Next: Run vbox_dump_memory.cmd to dump its memory
pause
