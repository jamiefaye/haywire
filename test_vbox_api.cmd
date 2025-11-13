@echo off
REM Test VirtualBox readPhysicalMemory API
REM This determines if we can build a memory bridge without recompiling VirtualBox

echo ========================================
echo VirtualBox Memory API Test
echo ========================================
echo.

REM Check if Python is installed
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found!
    echo Install Python from: https://www.python.org/downloads/
    pause
    exit /b 1
)

echo Python found:
python --version
echo.

REM Check if vboxapi is installed
python -c "import vboxapi" >nul 2>&1
if errorlevel 1 (
    echo Installing vboxapi module...
    pip install vboxapi
    if errorlevel 1 (
        echo ERROR: Failed to install vboxapi
        pause
        exit /b 1
    )
    echo.
)

REM List available VMs
echo Available VirtualBox VMs:
echo.
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" list vms
echo.

REM Prompt for VM name
set /p VM_NAME="Enter VM name to test (or press Ctrl+C to cancel): "

if "%VM_NAME%"=="" (
    echo ERROR: No VM name provided
    pause
    exit /b 1
)

echo.
echo Checking if VM is running...
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" showvminfo "%VM_NAME%" | findstr /C:"State:" | findstr /C:"running"
if errorlevel 1 (
    echo.
    echo VM is not running. Start it? (Y/N)
    set /p START_VM=""
    if /i "%START_VM%"=="Y" (
        echo Starting VM in headless mode...
        "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm "%VM_NAME%" --type headless
        if errorlevel 1 (
            echo ERROR: Failed to start VM
            pause
            exit /b 1
        )
        echo Waiting 10 seconds for VM to boot...
        timeout /t 10 /nobreak
    ) else (
        echo Please start the VM first:
        echo   VBoxManage startvm "%VM_NAME%" --type headless
        pause
        exit /b 1
    )
)

echo.
echo ========================================
echo Running API test...
echo ========================================
echo.

REM Run the Python test script
python test_vbox_memory_api.py "%VM_NAME%"

echo.
echo ========================================
echo Test complete!
echo ========================================
pause
