@echo off
REM Download kBuild using Git (GitHub mirror)

echo ========================================
echo Downloading kBuild via Git
echo ========================================
echo.

cd /d C:\Users\jamie\vbox-src

echo Cloning kBuild from GitHub mirror...
echo This may take 2-3 minutes...
echo.

git clone https://github.com/VirtualBox/kbuild.git kBuild

if errorlevel 1 (
    echo.
    echo ERROR: Git clone failed!
    echo.
    echo Please ensure Git is installed:
    echo   https://git-scm.com/download/win
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================
echo kBuild downloaded successfully!
echo ========================================
echo.
echo Location: C:\Users\jamie\vbox-src\kBuild
echo.
echo Next step: Run setup_vbox_build_env.cmd
echo.
pause
