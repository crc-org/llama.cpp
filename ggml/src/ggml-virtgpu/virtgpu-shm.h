#pragma once

#include <sys/mman.h>
#include <cstddef>
#include <cstdint>

// Include interface for complete type definitions
#include "virtgpu-interface.h"

#ifdef __cplusplus
extern "C" {
#endif

// Simplified cache coherency operations for Windows shared files
// These work with existing structure via backend_data pointer
// Note: virtgpu_shmem_create/destroy are declared in virtgpu-interface.h
void virtgpu_shmem_unmap_for_host(virtgpu_shmem * shmem);
int  virtgpu_shmem_remap_after_host(virtgpu_shmem * shmem, void * original_ptr);
void virtgpu_shmem_sync_to_host(virtgpu_shmem * shmem);
void virtgpu_shmem_sync_from_host(virtgpu_shmem * shmem);

#ifdef __cplusplus
}
#endif
