/*
 * Common VirtGPU Backend Interface
 *
 * This header defines the common interface that all VirtGPU backends must implement.
 * It allows for different transport mechanisms (Linux DRM, Windows winApiRmt, etc.)
 * to coexist in the same build.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <threads.h>

/* Forward declarations */
struct ggml_cgraph;
typedef struct apir_encoder apir_encoder;
typedef struct apir_decoder apir_decoder;

/* Backend types for selection */
typedef enum {
    VIRTGPU_BACKEND_LINUX_DRM = 1,
    VIRTGPU_BACKEND_WINDOWS_WINAPI = 2,
    VIRTGPU_BACKEND_AUTO = 0
} virtgpu_backend_type_t;

/* Common shared memory structure - backend-agnostic */
typedef struct {
    uint32_t res_id;         // Buffer ID for APIR protocol
    size_t   mmap_size;      // Size of mapped memory
    void *   mmap_ptr;       // Pointer to mapped memory

    /* Backend-specific data (opaque pointer) */
    void *   backend_data;
} virtgpu_shmem;

#ifdef GGML_VIRTGPU_USE_WINDOWS
#include "apir-windows.h"
#endif

/* Common utility array structure */
typedef struct {
    void** elements;
    size_t size;
    size_t capacity;
} util_sparse_array;

/* Forward declaration of the main virtgpu structure */
typedef struct virtgpu virtgpu;

/* Backend function table - each backend implements these functions */
typedef struct {
    const char* name;

    /* Lifecycle */
    virtgpu* (*create)(void);
    void (*destroy)(virtgpu* gpu);

    /* Core APIR functions */
    apir_encoder* (*remote_call_prepare)(virtgpu* gpu, int apir_cmd_type, int32_t cmd_flags);
    uint32_t (*remote_call)(virtgpu* gpu, apir_encoder* enc, apir_decoder** dec, uint64_t timeout_ms, long long* call_duration_ns);
    void (*remote_call_finish)(virtgpu* gpu, apir_encoder* enc, apir_decoder* dec);

    /* Shared memory operations */
    int (*shmem_create)(virtgpu* gpu, size_t size, virtgpu_shmem* shmem);
    void (*shmem_destroy)(virtgpu* gpu, virtgpu_shmem* shmem);
    void* (*shmem_get_ptr)(virtgpu_shmem* shmem);

    /* Utility functions */
    void (*sparse_array_init)(util_sparse_array* array, size_t element_size);
    void (*sparse_array_finish)(util_sparse_array* array);
    void* (*sparse_array_get)(util_sparse_array* array, uint64_t key);
    void (*sparse_array_set)(util_sparse_array* array, uint64_t key, void* element);
} virtgpu_backend_ops;

/* Main virtgpu structure - backend-agnostic */
struct virtgpu {
    /* Common fields */
    bool use_apir_capset;

    /* Backend identification */
    virtgpu_backend_type_t backend_type;
    const virtgpu_backend_ops* ops;

    /* Communication buffers */
    virtgpu_shmem reply_shmem;
    virtgpu_shmem data_shmem;

    /* Utility arrays */
    util_sparse_array shmem_array;

    /* Shared buffer synchronization */
    mtx_t data_shmem_mutex;

    /* Backend-specific data (opaque pointer) */
    void* backend_data;

    /* Cached device information to prevent memory leaks and race conditions */
    struct {
        char * description;
        char * name;
        int32_t device_count;
        uint32_t type;
        size_t memory_free;
        size_t memory_total;
    } cached_device_info;

    /* Cached buffer type information to prevent memory leaks and race conditions */
    struct {
        apir_buffer_type_host_handle_t host_handle;
        char * name;
        size_t alignment;
        size_t max_size;
        bool is_host;
    } cached_buffer_type;
};

/* Backend registration functions */
#ifndef GGML_VIRTGPU_USE_WINDOWS
const virtgpu_backend_ops* virtgpu_backend_linux_drm_get_ops(void);      // From virtgpu-linux-backend.c
#endif
#ifdef GGML_VIRTGPU_USE_WINDOWS
const virtgpu_backend_ops* virtgpu_backend_windows_winapi_get_ops(void); // From winApiRmt.c
#endif

/* Factory functions */
virtgpu* virtgpu_create_with_backend(virtgpu_backend_type_t backend_type);
virtgpu* create_virtgpu(void);  // Uses auto-detection or compile-time default

/* Common interface functions that delegate to backend ops */
struct apir_encoder* remote_call_prepare(virtgpu* gpu, int apir_cmd_type, int32_t cmd_flags);
uint32_t remote_call(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder** dec, uint64_t timeout_ms, long long* call_duration_ns);
void remote_call_finish(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder* dec);

int virtgpu_shmem_create(virtgpu* gpu, size_t size, virtgpu_shmem* shmem);
void virtgpu_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem);
void* virtgpu_shmem_get_ptr(virtgpu_shmem* shmem);

void util_sparse_array_init(util_sparse_array* array, size_t element_size);
void util_sparse_array_finish(util_sparse_array* array);
void* util_sparse_array_get(util_sparse_array* array, uint64_t key);
void util_sparse_array_set(util_sparse_array* array, uint64_t key, void* element);

#ifdef __cplusplus
}
#endif
