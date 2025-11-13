@echo off
REM VirtualBox Memory Dump Script for Windows
REM Usage: vbox_dump_memory.cmd <VM_NAME> [output_file]

setlocal enabledelayedexpansion

if "%~1"=="" (
    echo Usage: %0 ^<VM_NAME^> [output_file]
    echo.
    echo Available VMs:
    "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" list vms
    exit /b 1
)

set VM_NAME=%~1
set OUTPUT_FILE=%~2

REM Default output file if not specified
if "%OUTPUT_FILE%"=="" (
    set OUTPUT_FILE=C:\haywire-dumps\%VM_NAME%_memory_dump.elf
)

REM Create dumps directory
if not exist "C:\haywire-dumps" mkdir "C:\haywire-dumps"

echo ===============================================
echo VirtualBox Memory Dump Utility
echo ===============================================
echo VM Name: %VM_NAME%
echo Output:  %OUTPUT_FILE%
echo.

REM Check if VM is running
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" showvminfo "%VM_NAME%" | findstr /C:"State:" | findstr /C:"running" >nul
if errorlevel 1 (
    echo ERROR: VM "%VM_NAME%" is not running!
    echo.
    echo Start it with:
    echo   VBoxManage startvm "%VM_NAME%" --type headless
    exit /b 1
)

echo VM is running. Getting memory info...
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" showvminfo "%VM_NAME%" | findstr /C:"Memory size"

echo.
echo Dumping memory (this may take a while)...
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" debugvm "%VM_NAME%" dumpvmcore --filename "%OUTPUT_FILE%"

if errorlevel 1 (
    echo.
    echo ERROR: Memory dump failed!
    echo.
    echo Make sure:
    echo 1. VirtualBox Extension Pack is installed
    echo 2. VM was started with debugging enabled:
    echo    VBoxManage modifyvm "%VM_NAME%" --debug on
    exit /b 1
)

echo.
echo ===============================================
echo Success! Memory dump created:
echo %OUTPUT_FILE%
echo.

REM Show file size
for %%A in ("%OUTPUT_FILE%") do (
    set size=%%~zA
    set /a size_mb=!size! / 1048576
    echo Size: !size_mb! MB
)

echo.
echo Next steps:
echo 1. Parse dump: python parse_vbox_dump.py "%OUTPUT_FILE%" ubuntu_memory.bin
echo 2. Analyze with Haywire (once adapted for binary dumps)
echo ===============================================

endlocal
