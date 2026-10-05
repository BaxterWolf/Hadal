@echo off
rem Install or update Hadal
if not exist "%~dp0install.ps1" (
    echo Extract the whole zip first, then run Install.cmd from the extracted folder.
    pause
    exit /b 1
)
net session >nul 2>&1 || (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
echo.
pause
