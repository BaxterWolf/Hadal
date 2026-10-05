@echo off
rem Uninstall Hadal
if not exist "%~dp0uninstall.ps1" (
    echo Extract the whole zip first, then run Uninstall.cmd from the extracted folder.
    pause
    exit /b 1
)
net session >nul 2>&1 || (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
cd /d "%TEMP%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" & echo. & pause & (goto) 2>nul
