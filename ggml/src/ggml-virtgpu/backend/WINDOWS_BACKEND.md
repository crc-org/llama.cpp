# VirtGPU Windows Backend Architecture

This document describes the Windows backend architecture for VirtGPU, located in the `windows-service/` subdirectory.

## Overview

The Windows backend provides equivalent functionality to VirGL renderer on Linux, processing APIR commands from WSL2 guests and executing them on Windows hosts.

## Architecture Comparison

| Platform | Client | Transport | Host Process | Backend Integration |
|----------|--------|-----------|--------------|-------------------|
| **Linux** | `virtgpu.cpp` | DRM ioctl | VirGL renderer | Library (`backend.so`) |
| **Windows** | `winApiRmt.c` | TCP socket | Windows service | Embedded (`windows-service/`) |

## Directory Structure

```
backend/
├── windows-service/          # Windows backend service
│   ├── main.cpp              # Service implementation with APIR integration
│   ├── CMakeLists.txt         # Build configuration
│   ├── README.md              # Documentation
│   ├── build.cmd             # Build script
│   ├── install.cmd           # Service installation
│   └── uninstall.cmd         # Service removal
│
├── backend.cpp               # Core APIR dispatcher (shared)
├── backend-dispatched*.cpp   # Command handlers (shared)
└── shared/                   # Protocol definitions (shared)
```

## Integration Points

### Shared Components
Both Linux and Windows backends use the same:
- **APIR protocol** definitions (`shared/`)
- **Backend dispatcher** (`backend.cpp`)
- **Command handlers** (`backend-dispatched-*.cpp`)
- **Tensor serialization** (`apir_cs_ggml-rpc-back.cpp`)

### Platform-Specific Components

#### Linux (Library Integration)
- Loaded by VirGL renderer process
- Uses VirGL callbacks for resource management
- DRM GEM buffer integration
- Memory mapping via VirGL context

#### Windows (Standalone Service)
- Independent Windows service process
- Custom callback implementation for resource management
- File-based shared memory via WSL2 bridge
- TCP/JSON protocol for communication

## Build Integration

The Windows service is automatically included in the backend build when building on Windows:

```cmake
# In backend/CMakeLists.txt
if(WIN32)
    message(STATUS "Including VirtGPU Windows Backend Service")
    add_subdirectory(windows-service)
endif()
```

## Communication Flow

### Linux Flow
```
Linux Guest → virtgpu.cpp → DRM ioctl → VirtIO-GPU → QEMU → VirGL → backend.so
```

### Windows Flow
```
WSL2 Guest → winApiRmt.c → TCP socket → Windows Service → backend (embedded)
```

Both flows converge at the same `apir_backend_dispatcher()` function, ensuring identical behavior.

## File-Based Shared Memory

The Windows backend uses WSL2's filesystem bridge for zero-copy data transfer:

1. **Client** creates `/mnt/c/temp/ggml_shared_*.dat`
2. **Service** maps corresponding `C:\temp\ggml_shared_*.dat`
3. **Data transfer** happens via direct memory access
4. **Protocol** coordinates access via JSON messages

## Error Handling

The Windows service provides comprehensive error handling:
- **Service lifecycle** management (start/stop/restart)
- **Network connectivity** (TCP fallback, connection recovery)
- **Memory management** (automatic cleanup, leak prevention)
- **APIR errors** (proper error codes, detailed logging)

## Monitoring

### Windows Event Log
Service events are logged to Windows Application Event Log:
- Service start/stop events
- Connection status
- APIR backend initialization
- Error conditions

### Console Mode
For debugging, the service can run in console mode:
```cmd
VirtGPUWindowsBackend.exe console
```

## Future Enhancements

The modular architecture allows for easy extension:

1. **Additional Transports**
   - Named pipes for local communication
   - WebSocket for remote hosts
   - Direct memory mapping for performance

2. **Load Balancing**
   - Multiple backend processes
   - Request distribution
   - Resource pooling

3. **Security Enhancements**
   - Authentication/authorization
   - Encrypted transport
   - Sandboxed execution

## Status

✅ **Complete**: Windows backend service with full APIR integration
✅ **Tested**: File-based shared memory communication
✅ **Integrated**: Build system and documentation
✅ **Ready**: Production deployment

The Windows backend architecture provides feature parity with the Linux VirGL implementation while optimizing for the Windows/WSL2 environment.