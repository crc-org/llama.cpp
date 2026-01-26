# ✅ Backend Naming & Structure

## 🏷️ **New File Organization**

Following the user's request to make the backend naming more descriptive, we've reorganized the files to clearly reflect their purpose:

### **Linux Backend: `virtgpu.c/h`**
- **Purpose**: Linux DRM VirtGPU implementation
- **Transport**: Direct Linux DRM kernel interface
- **Files**:
  - `virtgpu.c` - Linux backend implementation
  - `virtgpu.h` - Linux backend header and data structures

### **Windows Backend: `winApiRmt.c/h`**
- **Purpose**: Windows API Remoting implementation
- **Transport**: TCP + JSON protocol over shared memory
- **Files**:
  - `winApiRmt.c` - Windows backend implementation
  - `winApiRmt.h` - Windows backend header and data structures

### **Common Infrastructure**
- `virtgpu-interface.h` - Common interface that both backends implement
- `virtgpu-common.cpp` - Dispatch layer that routes calls to backends
- `apir-minimal.h` - Minimal APIR encoder/decoder functions

## 📂 **File Structure Overview**

```
ggml-virtgpu/
├── Core Interface
│   ├── virtgpu-interface.h    # Common backend interface
│   ├── virtgpu-common.cpp     # Dispatch implementation
│   └── apir-minimal.h         # APIR encoder/decoder
│
├── Linux Backend (virtgpu)
│   ├── virtgpu.c              # Linux DRM implementation
│   └── virtgpu.h              # Linux structures & constants
│
├── Windows Backend (winApiRmt)
│   ├── winApiRmt.c            # Windows client implementation
│   ├── winApiRmt.h            # Windows structures & constants
│   └── ggml-winapi-client.c   # Standalone Windows client
│
├── Testing & Documentation
│   ├── test-backend-refactor.cpp
│   ├── BACKEND_REFACTORING.md
│   └── BACKEND_NAMING.md (this file)
│
└── Legacy (for migration reference)
    ├── virtgpu.cpp            # Original mixed implementation
    └── virtgpu-linux-original.cpp
```

## 🎯 **Naming Rationale**

### **`virtgpu.c/h` (Linux)**
- ✅ **Clear**: Immediately identifies as the original VirtGPU implementation
- ✅ **Historical**: Matches the Linux DRM subsystem naming
- ✅ **Concise**: Short and well-known in the VirtGPU community
- ✅ **Descriptive**: Directly relates to Linux VirtGPU drivers

### **`winApiRmt.c/h` (Windows)**
- ✅ **Descriptive**: Clearly indicates Windows API Remoting
- ✅ **Distinct**: Different from Linux, avoiding confusion
- ✅ **Accurate**: Reflects the actual transport mechanism
- ✅ **Expandable**: Can accommodate future Windows transport variations

## 🔧 **Backend Interface**

Both backends implement the same interface defined in `virtgpu-interface.h`:

```cpp
typedef struct {
    const char* name;

    /* Lifecycle */
    virtgpu* (*create)(void);
    void (*destroy)(virtgpu* gpu);

    /* Core APIR functions */
    apir_encoder* (*remote_call_prepare)(...);
    uint32_t (*remote_call)(...);
    void (*remote_call_finish)(...);

    /* Shared memory operations */
    int (*shmem_create)(...);
    void (*shmem_destroy)(...);
    void* (*shmem_get_ptr)(...);
} virtgpu_backend_ops;
```

## 📋 **Backend Registration**

Each backend provides a registration function:

```cpp
/* From virtgpu.h */
const virtgpu_backend_ops* virtgpu_backend_linux_drm_get_ops(void);

/* From winApiRmt.h */
const virtgpu_backend_ops* virtgpu_backend_windows_winapi_get_ops(void);
```

## 🚀 **Usage Examples**

### **Explicit Backend Selection:**
```cpp
#include "virtgpu-interface.h"

// Use Windows API Remoting
virtgpu* gpu = virtgpu_create_with_backend(VIRTGPU_BACKEND_WINDOWS_WINAPI);

// Use Linux DRM VirtGPU
virtgpu* gpu = virtgpu_create_with_backend(VIRTGPU_BACKEND_LINUX_DRM);
```

### **Auto-Detection (Default):**
```cpp
// Uses platform-appropriate backend automatically
virtgpu* gpu = create_virtgpu();
```

### **Direct Backend Access:**
```cpp
#include "winApiRmt.h"
#include "virtgpu.h"

// Get specific backend operations
const virtgpu_backend_ops* win_ops = virtgpu_backend_windows_winapi_get_ops();
const virtgpu_backend_ops* linux_ops = virtgpu_backend_linux_drm_get_ops();
```

## ✅ **Benefits of New Naming**

1. **Clarity** - File names immediately indicate purpose
2. **Separation** - Clear boundaries between Linux and Windows code
3. **Maintenance** - Easy to locate backend-specific issues
4. **Documentation** - Self-documenting file organization
5. **Development** - Teams can work on backends independently

## 📊 **Implementation Status**

| Backend | Implementation | Header | Registration | Status |
|---------|---------------|---------|-------------|---------|
| **Linux (virtgpu)** | `virtgpu.c` | `virtgpu.h` | ✅ Ready | ⚠️ Stub |
| **Windows (winApiRmt)** | `winApiRmt.c` | `winApiRmt.h` | ✅ Complete | ✅ Complete |
| **Common Interface** | `virtgpu-common.cpp` | `virtgpu-interface.h` | ✅ Complete | ✅ Ready |

## 🔄 **Migration Status**

- ✅ **File Renaming**: Complete
- ✅ **Header Structure**: Complete
- ✅ **Interface Registration**: Complete
- ✅ **Windows Implementation**: Complete
- ⚠️ **Linux Implementation**: Ready for migration from original code
- ✅ **Build System**: Updated in CMakeLists.txt
- ✅ **Testing Framework**: Updated and ready

## 🎉 **Result**

**Perfect naming scheme that clearly distinguishes between the Linux VirtGPU implementation (`virtgpu.c/h`) and Windows API Remoting implementation (`winApiRmt.c/h`) while maintaining a clean common interface!**

The refactored architecture is ready for development with clear, maintainable, and descriptive naming.