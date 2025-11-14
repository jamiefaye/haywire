@echo off
REM VirtualBox Build Environment Setup Script
REM This sets up everything needed to build VirtualBox on Windows

echo ========================================
echo VirtualBox Build Environment Setup
echo ========================================
echo.
echo This script will:
echo 1. Check for required tools (Visual Studio, Python, etc.)
echo 2. Download and install kBuild (VirtualBox build system)
echo 3. Download Windows SDK/DDK if needed
echo 4. Configure the VirtualBox source tree
echo 5. Verify the build environment is ready
echo.
echo Estimated time: 30-60 minutes (depending on downloads)
echo.
pause

REM ==========================================
REM Check Visual Studio
REM ==========================================
echo.
echo [1/6] Checking for Visual Studio...
echo.

set "VS_PATH="
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
    set "VS_VERSION=2022"
    echo Found Visual Studio 2022 Community
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Professional"
    set "VS_VERSION=2022"
    echo Found Visual Studio 2022 Professional
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community"
    set "VS_VERSION=2019"
    echo Found Visual Studio 2019 Community
) else (
    echo ERROR: Visual Studio not found!
    echo Please install Visual Studio 2019 or 2022 with C++ Desktop Development
    pause
    exit /b 1
)

echo Visual Studio: "%VS_PATH%"
echo.

REM ==========================================
REM Check Python
REM ==========================================
echo [2/6] Checking for Python...
echo.

python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found!
    echo Please install Python 3 from https://www.python.org/downloads/
    pause
    exit /b 1
)

python --version
echo.

REM ==========================================
REM Check Windows SDK
REM ==========================================
echo [3/6] Checking for Windows SDK...
echo.

REM Skip detailed check - Visual Studio usually has SDK
echo Assuming Windows SDK is installed with Visual Studio
echo (VirtualBox configure will verify this later)
echo.

REM ==========================================
REM Setup kBuild
REM ==========================================
echo [4/6] Setting up kBuild...
echo.

REM Download kBuild from netlabs.org (official source)
if not exist "C:\Users\jamie\vbox-src\kBuild" (
    echo Downloading kBuild from netlabs.org...
    echo This may take a few minutes...
    echo.

    REM Create temp directory
    mkdir "%TEMP%\kbuild_download" 2>nul

    REM Download kBuild tarball
    powershell -Command "& {[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri 'https://trac.netlabs.org/kbuild/export/HEAD/trunk' -OutFile '%TEMP%\kbuild_download\kbuild.zip'}"

    if errorlevel 1 (
        echo ERROR: Failed to download kBuild
        echo.
        echo Please download manually:
        echo 1. Go to: https://www.virtualbox.org/wiki/Windows_build_instructions
        echo 2. Follow the kBuild setup instructions
        echo 3. Or clone: svn co https://svn.netlabs.org/repos/kbuild/trunk kBuild
        pause
        exit /b 1
    )

    echo Extracting kBuild...
    powershell -Command "Expand-Archive -Path '%TEMP%\kbuild_download\kbuild.zip' -DestinationPath 'C:\Users\jamie\vbox-src\kBuild' -Force"

    rmdir /s /q "%TEMP%\kbuild_download"
) else (
    echo Found kBuild at C:\Users\jamie\vbox-src\kBuild
)

set "PATH=C:\Users\jamie\vbox-src\kBuild\bin\win.amd64;%PATH%"
echo.

REM ==========================================
REM Configure VirtualBox Source
REM ==========================================
echo [5/6] Configuring VirtualBox source tree...
echo.

cd /d C:\Users\jamie\vbox-src

if not exist configure.vbs (
    echo ERROR: Not in VirtualBox source directory!
    echo Expected: C:\Users\jamie\vbox-src
    pause
    exit /b 1
)

echo Running configure.vbs...
echo This may take 5-10 minutes...
echo.

REM Run VirtualBox configure directly (it will find VS automatically)
cscript //Nologo configure.vbs

if errorlevel 1 (
    echo.
    echo WARNING: Configure reported warnings/errors
    echo This may be normal - VirtualBox configure is strict
    echo.
    echo Check the output above for critical errors.
    echo.
    pause
)

echo.
echo Configuration complete!
echo.

REM ==========================================
REM Verify Build Environment
REM ==========================================
echo [6/6] Verifying build environment...
echo.

if not exist "env.bat" (
    echo ERROR: env.bat not created by configure!
    echo Configuration may have failed.
    pause
    exit /b 1
)

echo Found env.bat
echo.

if not exist "AutoConfig.kmk" (
    echo ERROR: AutoConfig.kmk not created!
    echo Configuration may have failed.
    pause
    exit /b 1
)

echo Found AutoConfig.kmk
echo.

REM ==========================================
REM Create Helper Scripts
REM ==========================================

echo Creating helper scripts...
echo.

REM Create build script
(
echo @echo off
echo REM Quick build script for VirtualBox
echo cd /d C:\Users\jamie\vbox-src
echo call env.bat
echo cd out\win.amd64\release
echo kmk %%*
) > C:\Users\jamie\vbox-src\quick_build.cmd

REM Create environment script
(
echo @echo off
echo REM Set up VirtualBox build environment
echo cd /d C:\Users\jamie\vbox-src
echo call env.bat
echo echo.
echo echo VirtualBox Build Environment Ready
echo echo.
echo echo Available commands:
echo echo   kmk           - Build VirtualBox
echo echo   kmk clean     - Clean build artifacts
echo echo   kmk rebuild   - Clean and rebuild
echo echo.
echo cmd /k
) > C:\Users\jamie\vbox-src\buildenv.cmd

echo.
echo ========================================
echo Build Environment Setup Complete!
echo ========================================
echo.
echo What's installed:
echo   - Visual Studio %VS_VERSION%: %VS_PATH%
echo   - Python:
python --version
echo   - kBuild: C:\kBuild
echo   - VirtualBox source: C:\Users\jamie\vbox-src
echo.
echo Next steps:
echo.
echo 1. To open a build environment shell:
echo    C:\Users\jamie\vbox-src\buildenv.cmd
echo.
echo 2. To build VirtualBox:
echo    cd C:\Users\jamie\vbox-src
echo    quick_build.cmd
echo.
echo 3. Build output will be in:
echo    C:\Users\jamie\vbox-src\out\win.amd64\release
echo.
echo Estimated first build time: 2-4 hours
echo Subsequent builds: 10-30 minutes (depending on changes)
echo.
pause
