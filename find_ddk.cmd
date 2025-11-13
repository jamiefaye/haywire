@echo off
REM Find where WDK 7.1 installed

echo Searching for WDK 7.1 installation...
echo.

REM Check common locations
echo Checking C:\WinDDK\
dir /b "C:\WinDDK\" 2>nul
if errorlevel 1 echo   Not found

echo.
echo Checking C:\Program Files\
dir /b /s "C:\Program Files\*7600*" 2>nul | findstr /i "inc lib bin"

echo.
echo Checking C:\Program Files (x86)\
dir /b /s "C:\Program Files (x86)\*7600*" 2>nul | findstr /i "inc lib bin"

echo.
echo Checking for WDK registry keys...
reg query "HKLM\SOFTWARE\Microsoft\KitSetup\configured-kits" 2>nul
reg query "HKLM\SOFTWARE\Wow6432Node\Microsoft\KitSetup\configured-kits" 2>nul

echo.
echo If you see a path above with inc, lib, and bin folders, that's where it installed!
echo.
pause
