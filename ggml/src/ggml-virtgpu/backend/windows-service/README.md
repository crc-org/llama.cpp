# VirtGPU Windows Backend Service

This directory contains the Windows backend service for VirtGPU, providing APIR command processing for Windows hosts communicating with Linux WSL2 guests.

## Architecture

The Windows backend service acts as the host-side equivalent of VirGL renderer on Linux:

- **Linux**: `virtgpu.cpp` → DRM ioctl → VirGL renderer → `apir_backend_dispatcher()`
- **Windows**: `winApiRmt.c` → TCP socket → **Windows service** → `apir_backend_dispatcher()`

## Components

### Core Service (`main.cpp`)
- TCP/Hyper-V socket server for client connections
- JSON protocol processing
- File-based shared memory management
- APIR backend integration via `apir_backend_dispatcher()`
- Windows service lifecycle management

### Communication Flow
1. **Client** (WSL2) creates shared memory file in `/mnt/c/temp/`
2. **Client** sends JSON command with file path over TCP socket
3. **Service** maps Windows file path (`C:\temp\`)
4. **Service** calls `apir_backend_dispatcher()` with mapped memory
5. **Service** returns JSON response to client

## Building

### Prerequisites
- Windows 10+ SDK
- vcpkg (for jsoncpp dependency)
- Visual Studio 2019+ or compatible C++17 compiler

### Install Dependencies
```cmd
vcpkg install jsoncpp:x64-windows
```

### Build Service
```cmd
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

## Installation

### Manual Installation
```cmd
# Copy executable to system location
copy VirtGPUWindowsBackend.exe C:\Program Files\VirtGPU\

# Install as Windows service
install.cmd
```

### Manual Service Registration
```cmd
sc create VirtGPUBackend binPath="C:\Program Files\VirtGPU\VirtGPUWindowsBackend.exe" start=auto
sc start VirtGPUBackend
```

## Configuration

### Environment Variables
- `WINAPI_SHARED_BASE`: Base path for shared memory files (default: `C:\temp`)
- `APIR_LLAMA_CPP_LOG_TO_FILE`: Enable APIR backend logging to file

### Network Settings
- **TCP Port**: `4660` (configurable)
- **Hyper-V Socket**: GUID `{00000000-facb-11e6-bd58-64006a7986d3}`, Port `0x400`

### Shared Memory
- **Location**: `C:\temp\ggml_shared_*` files
- **Access**: WSL2 filesystem bridge via `/mnt/c/temp/`
- **Size**: Up to 256MB per buffer

## Usage

### Service Control
```cmd
# Start service
sc start VirtGPUBackend

# Stop service
sc stop VirtGPUBackend

# Check status
sc query VirtGPUBackend

# View logs (console mode)
VirtGPUWindowsBackend.exe console
```

### Client Connection (from WSL2)
```bash
# Test connectivity
telnet <windows-host-ip> 4660

# Environment setup
export WINAPI_HOST=<windows-host-ip>
export WINAPI_PORT=4660
```

## API Protocol

### JSON Request Format
```json
{
    "api": "apir",
    "request_id": 1,
    "apir_cmd_type": 22,
    "apir_data_size": 16777216,
    "shared_file_path": "/mnt/c/temp/ggml_shared_1_16777216.dat",
    "buffer_id": 1
}
```

### JSON Response Format
```json
{
    "status": "success",
    "request_id": 1,
    "result": {
        "cmd_type": 22,
        "dispatch_result": 0,
        "response_size": 1234,
        "status": "success"
    }
}
```

## APIR Command Types

The service processes all 23 APIR command types:

- **Device Operations** (0-9): Device queries, capabilities
- **Buffer Type Operations** (10-15): Memory management
- **Buffer Operations** (16-21): Tensor data transfer
- **Backend Operations** (22): Graph computation

## Troubleshooting

### Common Issues

1. **Service fails to start**
   - Check Windows Event Log for detailed errors
   - Verify jsoncpp is properly installed
   - Ensure port 4660 is not in use

2. **Client connection refused**
   - Verify Windows Firewall allows port 4660
   - Check service is running: `sc query VirtGPUBackend`
   - Test with telnet from WSL2

3. **Shared memory access denied**
   - Ensure `C:\temp\` directory exists and is writable
   - Check WSL2 filesystem bridge is working: `ls /mnt/c/temp/`

4. **APIR backend initialization fails**
   - Verify GGML backend library dependencies
   - Check APIR backend logs for specific errors
   - Ensure backend architecture matches (x64)

### Debug Mode
```cmd
# Run in console mode for debugging
VirtGPUWindowsBackend.exe console

# Enable verbose logging
set APIR_LLAMA_CPP_LOG_TO_FILE=1
VirtGPUWindowsBackend.exe console
```

### Logs Location
- **Service logs**: Windows Event Log → Application
- **Console logs**: stdout when running in console mode
- **APIR logs**: File specified by `APIR_LLAMA_CPP_LOG_TO_FILE`

## Integration

This Windows service completes the cross-platform VirtGPU architecture:

- **Linux backend** works via existing VirGL renderer integration
- **Windows backend** works via this standalone service
- **Same APIR protocol** ensures identical functionality
- **Runtime selection** allows single applications to work on both platforms

The service integrates seamlessly with the VirtGPU client interface architecture, providing Windows hosts the equivalent functionality to Linux VirGL renderer hosts.