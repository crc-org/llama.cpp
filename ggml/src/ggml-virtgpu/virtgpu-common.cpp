/*
 * Common VirtGPU Backend Implementation
 *
 * This file provides the common interface implementation that dispatches
 * to the appropriate backend based on the virtgpu instance.
 */

#include "virtgpu-interface.h"
#include "apir-minimal.h"
#include "backend/shared/api_remoting.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Backend selection and auto-detection */
static virtgpu_backend_type_t detect_best_backend(void) {
#ifdef GGML_VIRTGPU_USE_WINDOWS
    /* Windows build - force WinAPI backend */
    GGML_LOG_INFO("Detected Windows build, using WINAPI backend\n");
    return VIRTGPU_BACKEND_WINDOWS_WINAPI;
#elif defined(__linux__)
    /* On Linux, prefer DRM backend if available */
    GGML_LOG_INFO("Detected Linux build, using DRM backend\n");
    return VIRTGPU_BACKEND_LINUX_DRM;
#elif defined(_WIN32)
    /* On Windows, use WinAPI backend */
    GGML_LOG_INFO("Detected _WIN32, using WINAPI backend\n");
    return VIRTGPU_BACKEND_WINDOWS_WINAPI;
#else
    /* Fallback - try to determine at runtime */
    GGML_LOG_ERROR("Unsupported platform for virtgpu auto-detection\n");
    return VIRTGPU_BACKEND_LINUX_DRM;  // Default fallback
#endif
}

static const virtgpu_backend_ops* get_backend_ops(virtgpu_backend_type_t backend_type) {
    switch (backend_type) {
#ifndef GGML_VIRTGPU_USE_WINDOWS
        case VIRTGPU_BACKEND_LINUX_DRM:
            return virtgpu_backend_linux_drm_get_ops();
#endif
#ifdef GGML_VIRTGPU_USE_WINDOWS
        case VIRTGPU_BACKEND_WINDOWS_WINAPI:
            return virtgpu_backend_windows_winapi_get_ops();
#endif
        case VIRTGPU_BACKEND_AUTO:
            return get_backend_ops(detect_best_backend());
        default:
            GGML_LOG_ERROR("Unknown virtgpu backend type: %d\n", backend_type);
            return NULL;
    }
}

/* Factory functions */
virtgpu* virtgpu_create_with_backend(virtgpu_backend_type_t backend_type) {
    const virtgpu_backend_ops* ops = get_backend_ops(backend_type);
    if (!ops) {
        GGML_LOG_ERROR("Failed to get backend operations for type %d\n", backend_type);
        return NULL;
    }

    GGML_LOG_INFO("Creating virtgpu with backend: %s\n", ops->name);

    virtgpu* gpu = ops->create();
    if (!gpu) {
        GGML_LOG_ERROR("Backend %s failed to create virtgpu instance\n", ops->name);
        return NULL;
    }

    /* Set backend information */
    gpu->backend_type = backend_type;
    gpu->ops = ops;

    return gpu;
}

virtgpu* create_virtgpu(void) {
    /* Use auto-detection by default */
    return virtgpu_create_with_backend(VIRTGPU_BACKEND_AUTO);
}

/* Common interface functions that delegate to backend ops */
struct apir_encoder* remote_call_prepare(virtgpu* gpu, int apir_cmd_type, int32_t cmd_flags) {
    if (!gpu || !gpu->ops || !gpu->ops->remote_call_prepare) {
        GGML_LOG_ERROR("Invalid virtgpu or missing remote_call_prepare implementation\n");
        return NULL;
    }
    return gpu->ops->remote_call_prepare(gpu, apir_cmd_type, cmd_flags);
}

uint32_t remote_call(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder** dec, uint64_t timeout_ms, long long* call_duration_ns) {
    if (!gpu || !gpu->ops || !gpu->ops->remote_call) {
        GGML_LOG_ERROR("Invalid virtgpu or missing remote_call implementation\n");
        return APIR_FORWARD_INVALID_ARGUMENT;
    }
    return gpu->ops->remote_call(gpu, enc, dec, timeout_ms, call_duration_ns);
}

void remote_call_finish(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder* dec) {
    if (!gpu || !gpu->ops || !gpu->ops->remote_call_finish) {
        GGML_LOG_ERROR("Invalid virtgpu or missing remote_call_finish implementation\n");
        return;
    }
    gpu->ops->remote_call_finish(gpu, enc, dec);
}

int virtgpu_shmem_create(virtgpu* gpu, size_t size, virtgpu_shmem* shmem) {
    if (!gpu || !gpu->ops || !gpu->ops->shmem_create) {
        GGML_LOG_ERROR("Invalid virtgpu or missing shmem_create implementation\n");
        return -1;
    }
    return gpu->ops->shmem_create(gpu, size, shmem);
}

void virtgpu_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem) {
    if (!gpu || !gpu->ops || !gpu->ops->shmem_destroy) {
        GGML_LOG_ERROR("Invalid virtgpu or missing shmem_destroy implementation\n");
        return;
    }
    gpu->ops->shmem_destroy(gpu, shmem);
}

void* virtgpu_shmem_get_ptr(virtgpu_shmem* shmem) {
    if (!shmem) {
        return NULL;
    }
    return shmem->mmap_ptr;
}

void util_sparse_array_init(util_sparse_array* array, size_t element_size) {
    if (!array) {
        GGML_LOG_ERROR("Invalid sparse array\n");
        return;
    }
    array->elements = NULL;
    array->size = 0;
    array->capacity = 0;
    (void)element_size; // Unused in this implementation
}

void util_sparse_array_finish(util_sparse_array* array) {
    if (!array) {
        return;
    }
    if (array->elements) {
        free(array->elements);
    }
    array->elements = NULL;
    array->size = 0;
    array->capacity = 0;
}

void* util_sparse_array_get(util_sparse_array* array, uint64_t key) {
    if (!array || key >= array->size) {
        return NULL;
    }
    return array->elements[key];
}

void util_sparse_array_set(util_sparse_array* array, uint64_t key, void* element) {
    if (!array) {
        return;
    }

    if (key >= array->capacity) {
        size_t new_capacity = key + 16;
        array->elements = (void**)realloc(array->elements, new_capacity * sizeof(void*));
        if (!array->elements) {
            GGML_LOG_ERROR("Failed to resize sparse array\n");
            return;
        }

        // Zero new elements
        for (size_t i = array->capacity; i < new_capacity; i++) {
            array->elements[i] = NULL;
        }

        array->capacity = new_capacity;
    }

    array->elements[key] = element;
    if (key >= array->size) {
        array->size = key + 1;
    }
}
