@echo off
REM Uninstallation script for Windows API Remoting Service
REM Must be run as Administrator

echo Uninstalling Windows API Remoting Service...
echo =============================================

REM Check if running as administrator
net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: This script must be run as Administrator
    echo Right-click and select "Run as administrator"
    exit /b 1
)

REM Stop the service
echo Stopping service...
net stop WinApiRemoting

if errorlevel 1 (
    echo Service was not running or not found
) else (
    echo Service stopped successfully
    timeout /t 3 /nobreak >nul
)

REM Delete the service
echo Removing service registration...
sc delete WinApiRemoting

if errorlevel 1 (
    echo ERROR: Failed to remove service
    echo The service may not be installed or may still be running
    exit /b 1
) else (
    echo Service removed successfully
)

echo.
echo Service uninstalled successfully!

echo.
echo Manual cleanup (optional):
echo =========================
echo 1. Delete service files: %CD%
echo 2. Remove shared memory directory: C:\temp (if not used by other applications)
echo 3. Check Event Log for any remaining entries

echo.
echo The service has been completely removed from the system.