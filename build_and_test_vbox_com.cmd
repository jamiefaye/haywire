@echo off
REM Build and test VirtualBox COM API access
REM This tells us if we can access guest memory without recompiling VirtualBox

echo ========================================
echo Building VirtualBox COM API Test
echo ========================================
echo.

REM Check if Visual Studio's cl.exe is available
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo Looking for Visual Studio...
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo ERROR: Visual Studio not found!
        echo Please run this from a "Visual Studio Developer Command Prompt"
        pause
        exit /b 1
    )
)

echo ✓ Visual Studio found
echo.

REM Check if VirtualBox SDK exists
if not exist "C:\Program Files\Oracle\VirtualBox\sdk" (
    echo ERROR: VirtualBox SDK not found!
    echo.
    echo The SDK should be at: C:\Program Files\Oracle\VirtualBox\sdk
    echo.
    echo To install it:
    echo 1. Download VirtualBox SDK from https://www.virtualbox.org/wiki/Downloads
    echo 2. Extract it to C:\Program Files\Oracle\VirtualBox\sdk
    echo.
    pause
    exit /b 1
)

echo ✓ VirtualBox SDK found
echo.

REM Compile
echo Compiling test program...
cl /EHsc /Fe:test_vbox_com.exe test_vbox_com.cpp ole32.lib oleaut32.lib >build.log 2>&1

if errorlevel 1 (
    echo ERROR: Compilation failed!
    echo.
    type build.log
    pause
    exit /b 1
)

echo ✓ Compiled successfully
echo.

REM List available VMs
echo Available VMs:
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" list vms
echo.

REM Run test
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
        echo Starting VM...
        "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm "%VM_NAME%" --type headless
        if errorlevel 1 (
            echo ERROR: Failed to start VM
            pause
            exit /b 1
        )
        echo Waiting 15 seconds for VM to boot...
        timeout /t 15 /nobreak
    ) else (
        echo Please start the VM first
        pause
        exit /b 1
    )
)

echo.
echo ========================================
echo Running COM API Test
echo ========================================
echo.

REM Run with default x86_64 address (0x0)
test_vbox_com.exe "%VM_NAME%" 0x0 4096

echo.
echo ========================================
echo Test Complete!
echo ========================================
echo.

if errorlevel 1 (
    echo Test FAILED - ReadPhysicalMemory not implemented
    echo See docs\vbox_memory_bridge_design.md for alternatives
) else (
    echo Test SUCCEEDED - We can build a memory bridge!
    echo Next step: Implement the memory bridge daemon
)

pause
