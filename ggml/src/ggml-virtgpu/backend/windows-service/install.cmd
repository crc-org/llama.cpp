@echo off
REM Installation script for Windows API Remoting Service
REM Must be run as Administrator

echo Installing Windows API Remoting Service...
echo =========================================

REM Check if running as administrator
net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: This script must be run as Administrator
    echo Right-click and select "Run as administrator"
    exit /b 1
)

REM Debug: Show current directory and contents
echo Current directory: %CD%
echo Checking for service binary...

REM Check if service binary exists (CMake build location first)
set "SERVICE_BINARY="

if exist "build\Release\WinApiRemotingService.exe" (
    echo [FOUND] CMake build: build\Release\WinApiRemotingService.exe
    set "SERVICE_BINARY=build\Release\WinApiRemotingService.exe"
    goto :binary_found
)

if exist "WinApiRemotingService.exe" (
    echo [FOUND] Direct build: WinApiRemotingService.exe
    set "SERVICE_BINARY=WinApiRemotingService.exe"
    goto :binary_found
)

REM If we get here, no binary was found
echo [NOT FOUND] Service binary not found
echo.
echo Current directory contents:
dir
echo.
echo Checking build directory:
if exist "build" (
    echo build directory exists
    dir build
    if exist "build\Release" (
        echo build\Release directory exists
        dir build\Release
    ) else (
        echo build\Release directory does not exist
    )
) else (
    echo build directory does not exist
)
echo.
echo Expected locations:
echo   build\Release\WinApiRemotingService.exe (CMake build)
echo   WinApiRemotingService.exe (direct build)
echo Please run build.cmd first to compile the service
exit /b 1

:binary_found

REM Stop service if already running
echo Stopping existing service...
net stop WinApiRemoting 2>nul
if errorlevel 1 (
    echo Service was not running
) else (
    echo Service stopped successfully
    timeout /t 2 /nobreak >nul
)

REM Delete existing service
echo Removing existing service registration...
sc delete WinApiRemoting 2>nul
if errorlevel 1 (
    echo No existing service found
) else (
    echo Existing service removed
    timeout /t 2 /nobreak >nul
)

REM Get full path for service binary
set "SERVICE_PATH=%CD%\%SERVICE_BINARY%"

echo Service path: %SERVICE_PATH%

REM Create service
echo Creating service...
sc create WinApiRemoting ^
    binPath= "%SERVICE_PATH%" ^
    DisplayName= "Windows API Remoting for WSL2" ^
    start= auto ^
    depend= "Tcpip"

if errorlevel 1 (
    echo ERROR: Failed to create service
    exit /b 1
)

REM Configure service description
sc description WinApiRemoting "Provides API remoting capabilities for WSL2 guests using Hyper-V sockets and shared memory."

REM Configure service failure actions (restart on failure)
sc failure WinApiRemoting reset= 0 actions= restart/5000/restart/5000/restart/5000

echo.
echo Service installed successfully!

REM Create shared memory directory on Windows side
echo Creating shared memory directory...
if not exist "C:\temp" mkdir "C:\temp"

REM Set permissions on shared memory directory (allow WSL access)
echo Setting permissions for WSL access...
icacls "C:\temp" /grant "Everyone:(OI)(CI)F" /T 2>nul

echo.
echo Configuration completed!

echo.
echo Starting service...
net start WinApiRemoting

if errorlevel 1 (
    echo.
    echo WARNING: Service failed to start automatically
    echo You can start it manually with: net start WinApiRemoting
    echo Or check the Event Log for error details
) else (
    echo.
    echo SUCCESS: Service started successfully!
)

echo.
echo Installation Summary:
echo ====================
echo Service Name:     WinApiRemoting
echo Display Name:     Windows API Remoting for WSL2
echo Binary Path:      %SERVICE_PATH%
echo Startup Type:     Automatic
echo Status:           Check with "sc query WinApiRemoting"

echo.
echo Testing:
echo ========
echo 1. Test console mode: WinApiRemotingService.exe console
echo 2. Check service status: sc query WinApiRemoting
echo 3. View service logs: Event Viewer ^> Applications and Services Logs
echo 4. Test from WSL2: run client test application

echo.
echo Service management:
echo ==================
echo Start service:    net start WinApiRemoting
echo Stop service:     net stop WinApiRemoting
echo Restart service:  net stop WinApiRemoting ^&^& net start WinApiRemoting
echo Uninstall:        uninstall.cmd