# GGML-VirtGPU Platform Organization Proposal

## Executive Summary

This document proposes a cleaner file organization for the ggml-virtgpu implementation that clearly separates the **virtgpu-virgl main implementation** (Linux) from the **Windows implementation** while maintaining the existing compile-time switching capabilities.

## Current Structure Analysis

### Problems with Current Organization
1. **Mixed platform files at root level** - Linux and Windows specific files intermixed
2. **Unclear common vs platform-specific separation**
3. **Inconsistent Windows file placement** - some in `winApiRmt/`, others at root
4. **Difficult maintenance** - hard to see what files belong to which platform

### Current Compile-Time Switch (GOOD - Keep This!)
- `GGML_VIRTGPU_USE_WINDOWS` CMake option works well
- Clean preprocessor directives in headers
- Platform-specific dependency management (libdrm vs json-c)
- Proper source file lists for each platform

## Proposed Directory Structure

```
ggml-virtgpu/
├── CMakeLists.txt                    # Main build configuration (UPDATED)
├── build-windows.sh                  # Build convenience scripts
├── build-test.sh
│
├── common/                           # SHARED CODE (NEW DIRECTORY)
│   ├── ggml-remoting.h               # Main conditional header
│   ├── virtgpu-interface.h          # Backend abstraction interface
│   ├── virtgpu-common.cpp            # Runtime dispatch logic
│   ├── apir-minimal.h                # APIR encoder/decoder
│   ├── virtgpu-forward.gen.h         # Generated forward declarations
│   ├── virtgpu-forward-impl.h        # Forward implementations
│   ├── virtgpu-forward-device.cpp    # Device forwarding
│   ├── virtgpu-forward-buffer.cpp    # Buffer forwarding
│   ├── virtgpu-forward-buffer-type.cpp # Buffer type forwarding
│   └── virtgpu-forward-backend.cpp   # Backend forwarding
│
├── ggml-backend/                     # GGML INTEGRATION (NEW DIRECTORY)
│   ├── ggml-backend.cpp              # Core backend
│   ├── ggml-backend-buffer.cpp       # Buffer management
│   ├── ggml-backend-device.cpp       # Device management
│   ├── ggml-backend-reg.cpp          # Backend registration
│   └── ggml-backend-buffer-type.cpp  # Buffer type implementation
│
├── platforms/                       # PLATFORM IMPLEMENTATIONS (NEW)
│   │
│   ├── linux/                       # VIRTGPU-VIRGL MAIN IMPLEMENTATION
│   │   ├── virtgpu.cpp               # Core Linux DRM implementation
│   │   ├── virtgpu.h                 # Linux VirtGPU header
│   │   ├── virtgpu-utils.cpp         # Linux utility functions
│   │   ├── virtgpu-utils.h           # Linux utility headers
│   │   ├── virtgpu-shm.cpp           # Linux shared memory
│   │   ├── virtgpu-shm.h             # Linux shared memory header
│   │   ├── virtgpu-apir.h            # Linux APIR definitions
│   │   ├── virtgpu-linux-backend.c   # Linux backend adapter
│   │   └── apir_cs_ggml-rpc-front.cpp # Linux RPC frontend
│   │
│   └── windows/                     # WINDOWS IMPLEMENTATION
│       ├── winApiRmt.c              # Windows API Remoting core
│       ├── winApiRmt.h              # Windows API Remoting header
│       ├── ggml-winapi-client.c     # Windows client implementation
│       ├── ggml-winapi-client.h     # Windows client header
│       ├── apir-windows.h           # Windows APIR definitions
│       └── remoting/                # Windows remoting infrastructure
│           ├── guest/               # WSL2 guest-side client
│           ├── host/                # Windows host-side implementation
│           ├── common/              # Protocol definitions
│           ├── sdk/                 # Development kit
│           ├── tests/               # Test infrastructure
│           └── azure/               # Azure VM management
│
├── backend/                         # BACKEND HOST PROCESSING (KEEP AS-IS)
│   ├── backend.cpp                   # Core dispatcher
│   ├── backend-dispatched*.cpp      # APIR command handlers
│   ├── shared/                      # Common protocol definitions
│   └── windows-service/             # Windows backend service
│
├── tests/                           # TESTING (NEW DIRECTORY)
│   ├── test-build-mode.cpp          # Build mode verification
│   ├── test-winapi-integration.cpp  # Windows integration test
│   ├── test-apir-encoding.cpp       # APIR encoding validation
│   ├── test-backend-refactor.cpp    # Backend architecture test
│   └── test-integration-final.cpp   # Final integration test
│
└── docs/                            # DOCUMENTATION (NEW DIRECTORY)
    ├── INTEGRATION_SUMMARY.md       # High-level integration overview
    ├── BACKEND_REFACTORING.md       # Architecture refactoring
    ├── BUILD_SYSTEM_README.md       # Build system documentation
    └── structure-comparison.md      # Architecture comparison
```

## Key Changes

### 1. **Clear Platform Separation**
- `platforms/linux/` - All virtgpu-virgl (main) implementation files
- `platforms/windows/` - All Windows implementation files
- Move `winApiRmt/` content to `platforms/windows/remoting/`

### 2. **Shared Code Organization**
- `common/` - Code used by both platforms
- `ggml-backend/` - GGML integration layer (platform-agnostic)

### 3. **Clean Support Directories**
- `tests/` - All test files in one place
- `docs/` - All documentation in one place

## Updated CMakeLists.txt Logic

```cmake
# Platform-specific source file organization
if (GGML_VIRTGPU_USE_WINDOWS)
    set(PLATFORM_SOURCES
        # Windows implementation
        platforms/windows/winApiRmt.c
        platforms/windows/ggml-winapi-client.c
        platforms/windows/apir-windows.h
    )
    set(PLATFORM_INCLUDES platforms/windows)
    set(PLATFORM_LIBRARIES ${JSON_C_LIBRARIES})
else()
    set(PLATFORM_SOURCES
        # Linux virtgpu-virgl implementation
        platforms/linux/virtgpu.cpp
        platforms/linux/virtgpu-utils.cpp
        platforms/linux/virtgpu-shm.cpp
        platforms/linux/virtgpu-linux-backend.c
        platforms/linux/apir_cs_ggml-rpc-front.cpp
    )
    set(PLATFORM_INCLUDES platforms/linux)
    set(PLATFORM_LIBRARIES ${DRM_LIBRARIES})
endif()

set(VIRTGPU_SOURCES
    # Common/shared code
    common/virtgpu-interface.h
    common/virtgpu-common.cpp
    common/virtgpu-forward-device.cpp
    common/virtgpu-forward-buffer.cpp
    common/virtgpu-forward-buffer-type.cpp
    common/virtgpu-forward-backend.cpp

    # GGML backend integration
    ggml-backend/ggml-backend.cpp
    ggml-backend/ggml-backend-buffer.cpp
    ggml-backend/ggml-backend-device.cpp
    ggml-backend/ggml-backend-reg.cpp
    ggml-backend/ggml-backend-buffer-type.cpp

    # Platform-specific sources
    ${PLATFORM_SOURCES}
)
```

## Implementation Plan

### Phase 1: Directory Structure
1. Create new directories: `common/`, `ggml-backend/`, `platforms/linux/`, `platforms/windows/`, `tests/`, `docs/`
2. Move files to appropriate locations
3. Update all `#include` paths in source files

### Phase 2: CMakeLists.txt Updates
1. Update source file paths in CMakeLists.txt
2. Update include directory paths
3. Test both Windows and Linux builds

### Phase 3: Documentation Updates
1. Update all documentation to reflect new structure
2. Update README files
3. Update any scripts that reference file paths

### Phase 4: Testing & Validation
1. Verify Windows build: `cmake -DGGML_VIRTGPU_USE_WINDOWS=ON`
2. Verify Linux build: `cmake -DGGML_VIRTGPU_USE_WINDOWS=OFF`
3. Run all integration tests
4. Verify no functionality changes

## Benefits

### 1. **Clear Mental Model**
- Developers immediately understand: "Linux implementation is in `platforms/linux/`"
- Windows developers focus on `platforms/windows/`
- Common code clearly separated

### 2. **Easier Maintenance**
- Platform-specific changes isolated to platform directories
- Shared code changes affect both platforms predictably
- Easier to onboard new developers

### 3. **Better Build System**
- CMakeLists.txt logic becomes clearer
- Easier to add new platforms in future
- Include paths are more logical

### 4. **Scalability**
- Easy to add new platforms (e.g., `platforms/macos/`)
- Clear separation of concerns
- Better for CI/CD pipelines

## Migration Strategy

### Step 1: Gradual Migration (Low Risk)
1. Create new directory structure alongside existing
2. Copy files to new locations
3. Update CMakeLists.txt to use new paths
4. Test thoroughly
5. Remove old files once confirmed working

### Step 2: Path Updates
1. Use find/replace to update all `#include` statements
2. Update any hardcoded paths in scripts
3. Update documentation

### Step 3: Validation
1. Build and test both platforms
2. Run existing test suite
3. Verify no regressions

## Conclusion

This reorganization will make the ggml-virtgpu codebase much cleaner and easier to maintain while preserving the excellent compile-time switching mechanism you've already implemented. The virtgpu-virgl implementation remains the "main" implementation in `platforms/linux/`, while Windows support is clearly separated in `platforms/windows/`.

The benefits far outweigh the one-time cost of reorganizing the files.