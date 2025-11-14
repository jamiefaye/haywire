@echo off
echo Converting QCOW2 to VDI format...
echo This will take several minutes (17GB file)
echo.

"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" clonehd "\\wsl$\Ubuntu\home\jamie\haywire\vms\ubuntu_x86_64.qcow2" "C:\haywire-vbox\ubuntu_x86_64.vdi" --format VDI

if errorlevel 1 (
    echo.
    echo ERROR: Conversion failed!
    pause
    exit /b 1
)

echo.
echo SUCCESS! VDI created at C:\haywire-vbox\ubuntu_x86_64.vdi
pause
