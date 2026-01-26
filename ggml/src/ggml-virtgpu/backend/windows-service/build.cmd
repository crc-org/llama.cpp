@echo off
REM Build script for Windows API Remoting Service using CMake
REM Requires CMake, vcpkg, and Visual Studio Build Tools

echo Building Windows API Remoting Service with CMake...
echo ===================================================

REM Check for CMake
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: CMake not found in PATH
    echo.
    echo To install CMake:
    echo   1. Run: winget install Kitware.CMake
    echo   2. Restart PowerShell as Administrator
    echo   3. Or add to PATH temporarily in PowerShell: $env:PATH += ";C:\Program Files\CMake\bin"
    echo.
    exit /b 1
) else (
    echo [OK] CMake found
    cmake --version | findstr "cmake version"
)

REM Check for vcpkg
if not exist "C:\vcpkg\vcpkg.exe" (
    echo ERROR: vcpkg not found at C:\vcpkg\
    echo.
    echo To install vcpkg:
    echo   1. git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
    echo   2. cd C:\vcpkg
    echo   3. .\bootstrap-vcpkg.bat
    echo   4. .\vcpkg integrate install
    echo.
    exit /b 1
) else (
    echo [OK] vcpkg found at C:\vcpkg\
)

REM Check for required libraries
echo Checking dependencies...

REM Check for jsoncpp
if not exist "C:\vcpkg\installed\x64-windows\include\json\json.h" (
    echo ERROR: jsoncpp not found in C:\vcpkg\installed\x64-windows\
    echo.
    echo To install jsoncpp:
    echo   1. cd C:\vcpkg
    echo   2. .\vcpkg install jsoncpp:x64-windows
    echo.
    exit /b 1
) else (
    echo [OK] jsoncpp found
)

REM Check for vcpkg toolchain
if not exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
    echo ERROR: vcpkg CMake toolchain not found
    echo   Expected: C:\vcpkg\scripts\buildsystems\vcpkg.cmake
    exit /b 1
) else (
    echo [OK] vcpkg CMake toolchain found
)

echo.
echo Dependencies verified successfully!
echo.

REM Create and enter build directory
echo Creating build directory...
if not exist "build" mkdir build
cd build

REM Clean previous build
echo Cleaning previous build...
if exist "Release\WinApiRemotingService.exe" del "Release\WinApiRemotingService.exe"
if exist "Debug\WinApiRemotingService.exe" del "Debug\WinApiRemotingService.exe"
if exist "CMakeCache.txt" del "CMakeCache.txt"
if exist "CMakeFiles" rmdir /s /q "CMakeFiles"

REM Configure with CMake
echo.
echo Configuring project with CMake...
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

if errorlevel 1 (
    echo.
    echo ERROR: CMake configuration failed
    echo.
    echo Common solutions:
    echo   1. Ensure Visual Studio Build Tools are installed
    echo   2. Run this script from Visual Studio Developer Command Prompt
    echo   3. Install Windows SDK via Visual Studio Installer
    echo.
    cd ..
    exit /b 1
)

REM Build the project
echo.
echo Building project...
cmake --build . --config Release

if errorlevel 1 (
    echo.
    echo ERROR: Build failed
    echo.
    echo Check the error messages above for details.
    echo Common issues:
    echo   1. Missing Visual Studio C++ compiler
    echo   2. Missing Windows SDK
    echo   3. jsoncpp library not properly linked
    echo.
    cd ..
    exit /b 1
)

REM Check if executable was created
if not exist "Release\WinApiRemotingService.exe" (
    echo ERROR: WinApiRemotingService.exe was not created
    cd ..
    exit /b 1
)

echo.
echo SUCCESS: Service compiled successfully with CMake!
echo Binary: %CD%\Release\WinApiRemotingService.exe
echo Size:
for %%F in (Release\WinApiRemotingService.exe) do echo %%~zF bytes

echo.
echo Build artifacts:
dir Release\*.exe
dir Release\*.pdb 2>nul

REM Return to original directory
cd ..

echo.
echo Next steps:
echo   1. Run as Administrator: install.cmd
echo   2. Test: build\Release\WinApiRemotingService.exe console
echo   3. Install service: install.cmd
echo   4. Start service: net start WinAPIRemoting

echo.
echo Build completed successfully!