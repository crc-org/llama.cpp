@echo off
REM Basic Windows API Remoting Test Script
REM This script tests the VirtGPU Windows Backend Service

echo === Windows API Remoting Basic Test ===
echo.

REM Check if service is running
sc query VirtGPUBackend >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] VirtGPUBackend service is running
) else (
    echo [WARNING] VirtGPUBackend service is not running
    echo Please start the service with: sc start VirtGPUBackend
    echo.
)

REM Test connectivity with telnet (if available)
echo Testing connectivity to localhost:4660...
timeout /t 1 >nul
telnet localhost 4660 2>nul
if %errorlevel% equ 0 (
    echo [PASS] Service is accepting connections on port 4660
) else (
    echo [FAIL] Cannot connect to service on port 4660
)

REM Create temp directory for shared memory files
if not exist "C:\temp" (
    echo Creating C:\temp directory for shared memory files...
    mkdir "C:\temp"
)

if exist "C:\temp" (
    echo [PASS] Shared memory directory C:\temp is available
) else (
    echo [FAIL] Cannot access C:\temp directory
)

REM Test file creation in temp directory
echo test > "C:\temp\test_write.dat" 2>nul
if exist "C:\temp\test_write.dat" (
    echo [PASS] Can write to shared memory directory
    del "C:\temp\test_write.dat" >nul 2>&1
) else (
    echo [FAIL] Cannot write to shared memory directory
)

echo.
echo === Basic Test Complete ===
echo.
echo To run full integration tests:
echo 1. Ensure VirtGPUBackend service is running
echo 2. Build test-windows-api-remoting.exe with Visual Studio
echo 3. Run: test-windows-api-remoting.exe
echo.
pause