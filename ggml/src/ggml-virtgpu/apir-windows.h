#pragma once

#include <stdint.h>
#include <stddef.h>

/* Forward declare virtgpu - virtgpu_shmem definition comes from virtgpu-interface.h */
typedef struct virtgpu virtgpu;

/* Note: virtgpu_shmem must be defined before this file is used.
 * It should be included from virtgpu-interface.h */

/* Windows-compatible APIR types for ggml backend compatibility */
typedef uint64_t apir_buffer_host_handle_t;
#ifndef APIR_BUFFER_TYPE_HOST_HANDLE_T_DEFINED
#define APIR_BUFFER_TYPE_HOST_HANDLE_T_DEFINED
typedef uint64_t apir_buffer_type_host_handle_t;
#endif

typedef struct {
    apir_buffer_host_handle_t host_handle;
    virtgpu_shmem shmem;  /* Use full Windows virtgpu_shmem structure */
    apir_buffer_type_host_handle_t buft_host_handle;
} apir_buffer_context_t;

/* UNUSED macro for compatibility */
#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif
