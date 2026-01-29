# ggml-virtgpu Unified Build System

This directory now supports building ggml-virtgpu with either **Linux DRM** or **Windows winApiRmt** backends through conditional compilation.

## Build Options

### Linux DRM Backend (Default)
```bash
# Configure for Linux (default)
cmake -DGGML_VIRTGPU_USE_WINDOWS=OFF .

# Or simply
cmake .
```

### Windows winApiRmt Backend
```bash
# Configure for Windows
cmake -DGGML_VIRTGPU_USE_WINDOWS=ON .
```

## Quick Start

### For Windows Development:
```bash
# Build with Windows backend
./build-windows.sh

# Test the build
cd build-windows
./test-build-mode
```

### For Linux Development:
```bash
# Build with Linux backend (default)
mkdir build-linux
cd build-linux
cmake .. -DGGML_VIRTGPU_USE_WINDOWS=OFF
make
./test-build-mode
```

## File Structure

### Original Files (Backed Up)
- `virtgpu-linux-original.h` - Original Linux virtgpu header
- `virtgpu-linux-original.cpp` - Original Linux virtgpu implementation

### Unified Files (Active)
- `virtgpu.h` - Unified header with conditional compilation
- `virtgpu.cpp` - Unified implementation with conditional compilation
- `CMakeLists.txt` - Updated with Windows/Linux build options

### Windows-Specific Files
- `winapi-apir-protocol.h` - Extended winApiRmt protocol for APIR
- `winapi-apir-client.c` - APIR client over winApiRmt
- `virtgpu-unified.h` - Template for unified header structure
- `virtgpu-unified.cpp` - Template for unified implementation

### Test Files
- `test-build-mode.cpp` - Test which backend is active
- `test-winapi-integration.cpp` - Test winApiRmt integration
- `build-windows.sh` - Windows build script

## Build Dependencies

### For Windows Backend:
- **libjson-c-dev**: JSON protocol support
- **winApiRmt client library**: From winApiRmt/guest/client/
- **Standard C++ compiler**: g++ or clang++ with C++17

### For Linux Backend:
- **libdrm-dev**: DRM/VirtIO GPU support
- **Standard C++ compiler**: g++ or clang++ with C++17

## CMake Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `GGML_VIRTGPU_USE_WINDOWS` | `OFF` | Use Windows winApiRmt instead of Linux DRM |
| `GGML_VIRTGPU_BACKEND` | - | Backend configuration (existing) |
| `GGML_BACKEND_DL` | - | Dynamic loading configuration (existing) |

## Conditional Compilation

The build system uses the `GGML_VIRTGPU_USE_WINDOWS` preprocessor definition:

```cpp
#ifdef GGML_VIRTGPU_USE_WINDOWS
    // Windows winApiRmt implementation
    #include "winApiRmt/guest/client/libwinapi.h"
    // ...
#else
    // Linux DRM implementation
    #include <xf86drm.h>
    // ...
#endif
```

## Transport Layer Differences

| Aspect | Linux DRM | Windows winApiRmt |
|--------|-----------|-------------------|
| **Communication** | DRM ioctls | Hyper-V socket + TCP |
| **Shared Memory** | DRM GEM buffers | File-backed (/mnt/c/) |
| **Buffer Size** | Fixed 24MB | Dynamic allocation |
| **Protocol** | APIR binary | APIR over JSON wrapper |
| **Dependencies** | libdrm | json-c, winApiRmt |

## Testing

### 1. Build Mode Test
```bash
# Compile test program
g++ -DGGML_VIRTGPU_USE_WINDOWS test-build-mode.cpp -o test-build-mode

# Run test
./test-build-mode
```

**Expected Output (Windows):**
```
Backend: Windows winApiRmt
Transport: Hyper-V socket + TCP fallback
SUCCESS: Windows virtgpu created successfully
```

**Expected Output (Linux):**
```
Backend: Linux DRM
Transport: VirtIO GPU DRM ioctls
SUCCESS: Linux virtgpu created successfully
```

### 2. Integration Test (Windows Only)
```bash
# Build integration test
./build-test.sh

# Run integration test (requires winApiRmt service)
./test-winapi-integration
```

### 3. Full Build Test
```bash
# Test Windows build
./build-windows.sh

# Test Linux build
mkdir build-linux && cd build-linux
cmake .. -DGGML_VIRTGPU_USE_WINDOWS=OFF
make
```

## Runtime Configuration

### Environment Variables
```bash
# Enable APIR capset (both platforms)
export GGML_REMOTING_USE_APIR_CAPSET=1

# Use virtgpu backend
export GGML_BACKEND_DEVICE=virtgpu

# Debug logging
export GGML_LOG_LEVEL=DEBUG
```

### Windows-Specific Setup
1. **Start winApiRmt service** on Windows host
2. **Ensure network connectivity** between WSL2 and Windows
3. **Verify shared memory access** via `/mnt/c/` path

### Linux-Specific Setup
1. **Load virtgpu driver**: `modprobe virtio_gpu`
2. **Ensure DRM device exists**: `ls /dev/dri/`
3. **Check permissions**: User must have access to DRM devices

## Troubleshooting

### Windows Build Issues

**Error: json-c not found**
```bash
# Ubuntu/Debian
sudo apt-get install libjson-c-dev

# CentOS/RHEL
sudo yum install json-c-devel
```

**Error: winApiRmt library not found**
```bash
# Build winApiRmt client
cd winApiRmt
./build.sh

# Verify library exists
ls winApiRmt/guest/client/libwinapi.*
```

**Error: winapi_init() fails**
```
1. Check if winApiRmt Windows service is running
2. Test network connectivity: ping <windows-host>
3. Verify Hyper-V socket support in WSL2
```

### Linux Build Issues

**Error: libdrm not found**
```bash
# Ubuntu/Debian
sudo apt-get install libdrm-dev

# CentOS/RHEL
sudo yum install libdrm-devel
```

**Error: virtgpu device not found**
```bash
# Check if virtgpu is available
lsmod | grep virtio_gpu
ls /dev/dri/

# Load virtgpu driver if needed
sudo modprobe virtio_gpu
```

## Migration Guide

### From Manual File Copying to Build System

**Old Approach:**
```bash
# Manual file replacement
cp virtgpu.h virtgpu-linux-backup.h
cp virtgpu-windows.h virtgpu.h
```

**New Approach:**
```bash
# Conditional compilation
cmake -DGGML_VIRTGPU_USE_WINDOWS=ON .
make
```

### Switching Between Platforms

**To Windows:**
```bash
cd build-windows
cmake .. -DGGML_VIRTGPU_USE_WINDOWS=ON
make
```

**To Linux:**
```bash
cd build-linux
cmake .. -DGGML_VIRTGPU_USE_WINDOWS=OFF
make
```

## Integration with Larger Projects

This build system integrates cleanly with larger GGML/llama.cpp builds:

```bash
# In parent CMakeLists.txt
option(GGML_VIRTGPU_USE_WINDOWS "Use Windows winApiRmt transport" OFF)

# Pass down to ggml-virtgpu
add_subdirectory(src/ggml/src/ggml-virtgpu)
```

The same codebase can now target both platforms without manual file management.