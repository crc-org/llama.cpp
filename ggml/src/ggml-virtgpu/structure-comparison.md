# Structure Comparison: Linux vs Windows

## The Same `struct virtgpu` - Different Implementations

### Linux Implementation (DRM-based):
```cpp
#ifdef GGML_VIRTGPU_USE_WINDOWS == false
struct virtgpu {
    bool use_apir_capset;

    int fd;                           // DRM file descriptor

    struct {
        virgl_renderer_capset      id;
        uint32_t                   version;
        virgl_renderer_capset_apir data;
    } capset;

    util_sparse_array shmem_array;   // DRM memory management

    virtgpu_shmem reply_shmem;        // DRM GEM buffer
    virtgpu_shmem data_shmem;         // DRM GEM buffer
};
```

### Windows Implementation (winApiRmt-based):
```cpp
#ifdef GGML_VIRTGPU_USE_WINDOWS == true
struct virtgpu {
    bool use_apir_capset;             // Same field!

    winapi_handle_t winapi_handle;    // winApiRmt connection

    struct {
        uint32_t id;                  // Simplified
        uint32_t version;
        void* data;
    } capset;

    util_sparse_array shmem_array;   // Simple array implementation

    virtgpu_shmem reply_shmem;        // winApiRmt shared buffer
    virtgpu_shmem data_shmem;         // winApiRmt shared buffer
};
```

### `virtgpu_shmem` Abstraction:

**Linux (DRM GEM):**
```cpp
// From virtgpu-shm.h (original)
typedef struct {
    uint32_t handle;       // DRM GEM handle
    uint64_t offset;       // DRM mapping offset
    size_t size;
    void* ptr;             // mmap() result
} virtgpu_shmem;
```

**Windows (File-backed):**
```cpp
// From our Windows implementation
typedef struct {
    winapi_shared_buffer_t buffer;  // winApiRmt buffer
    void* mapped_ptr;               // Points to buffer.data
    size_t size;
} virtgpu_shmem;
```