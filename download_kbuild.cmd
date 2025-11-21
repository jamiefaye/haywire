@echo off
REM Download kBuild for VirtualBox build

echo ========================================
echo Downloading kBuild
echo ========================================
echo.

cd /d C:\Users\jamie\vbox-src

REM Check if SVN is available
svn --version >nul 2>&1
if not errorlevel 1 (
    echo SVN found, checking out kBuild...
    svn co https://svn.netlabs.org/repos/kbuild/trunk kBuild
    if not errorlevel 1 (
        echo.
        echo kBuild downloaded successfully!
        pause
        exit /b 0
    )
)

echo.
echo SVN not available or checkout failed.
echo.
echo Manual download options:
echo.
echo Option 1: Install TortoiseSVN and checkout
echo   URL: https://svn.netlabs.org/repos/kbuild/trunk
echo   To: C:\Users\jamie\vbox-src\kBuild
echo.
echo Option 2: Download pre-built kBuild
echo   1. Download from: ftp://ftp.netlabs.org/pub/kbuild/kBuild-0.1.5-p2.tar.gz
echo   2. Extract to: C:\Users\jamie\vbox-src\kBuild
echo.
echo Option 3: Use Git mirror
echo   git clone https://github.com/VirtualBox/kbuild.git kBuild
echo.
pause
