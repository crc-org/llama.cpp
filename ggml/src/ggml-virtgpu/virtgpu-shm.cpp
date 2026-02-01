#include "virtgpu-shm.h"
#include "virtgpu-interface.h"  // For complete type definitions
#include "ggml-winapi-client.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

// Local definition to avoid include conflicts - matches virtgpu-interface.h
typedef struct {
    uint32_t res_id;
    size_t   mmap_size;
    void *   mmap_ptr;
    void *   backend_data;
} local_virtgpu_shmem;

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

// VirtGPU functions removed - Windows uses winapi shared buffers

// virtgpu_shmem_destroy is implemented in virtgpu-common.cpp (interface wrapper)

// Simplified cache coherency operations working with existing backend_data structure

extern "C" {

// Guest side: Unmap memory and close FD to flush data for host access
void virtgpu_shmem_unmap_for_host(virtgpu_shmem * shmem) {
    local_virtgpu_shmem * local_shmem = (local_virtgpu_shmem *)shmem;
    if (!local_shmem || !local_shmem->backend_data) {
        return;
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)local_shmem->backend_data;

    // Flush writes before unmapping
    if (local_shmem->mmap_ptr && local_shmem->mmap_size > 0) {
        if (msync(local_shmem->mmap_ptr, local_shmem->mmap_size, MS_SYNC) != 0) {
            printf("FATAL: msync MS_SYNC failed for buffer: %s\n", strerror(errno));
            exit(1);
        }

        // Unmap memory
        if (munmap(local_shmem->mmap_ptr, local_shmem->mmap_size) != 0) {
            printf("FATAL: munmap failed for buffer %p: %s\n", local_shmem->mmap_ptr, strerror(errno));
            exit(1);
        }
        local_shmem->mmap_ptr = NULL;
    }

    // Close FD to flush to disk for host
    if (winapi_buf->fd >= 0) {
        close(winapi_buf->fd);
        winapi_buf->fd = -1;
    }
}

// Guest side: Reopen file and remap at original address to see host changes
int virtgpu_shmem_remap_after_host(virtgpu_shmem * shmem, void * original_ptr) {
    local_virtgpu_shmem * local_shmem = (local_virtgpu_shmem *)shmem;
    if (!local_shmem || !local_shmem->backend_data) {
        return 0;
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)local_shmem->backend_data;

    // Reopen file with fresh FD to see host changes
    if (winapi_buf->file_path[0]) {
        int fresh_fd = open(winapi_buf->file_path, O_RDWR);
        if (fresh_fd < 0) {
            printf("FATAL: Failed to reopen file after host operation: %s\n", strerror(errno));
            exit(1);
        }
        winapi_buf->fd = fresh_fd;

        // Remap at original address - must succeed at same address
        void * remapped = mmap(original_ptr, local_shmem->mmap_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fresh_fd, 0);

        if (remapped == MAP_FAILED || remapped != original_ptr) {
            printf("FATAL: Failed to remap buffer at original address %p: %s\n",
                   original_ptr, strerror(errno));
            exit(1);
        }

        local_shmem->mmap_ptr = remapped;

        // Invalidate cache to see host changes
        if (msync(remapped, local_shmem->mmap_size, MS_INVALIDATE) != 0) {
            printf("FATAL: msync MS_INVALIDATE failed: %s\n", strerror(errno));
            exit(1);
        }
    }

    return 0;
}

// Guest side: Sync data TO host (flush writes)
void virtgpu_shmem_sync_to_host(virtgpu_shmem * shmem) {
    local_virtgpu_shmem * local_shmem = (local_virtgpu_shmem *)shmem;
    if (!local_shmem || !local_shmem->mmap_ptr) return;

    if (msync(local_shmem->mmap_ptr, local_shmem->mmap_size, MS_SYNC) != 0) {
        printf("FATAL: sync to host failed: %s\n", strerror(errno));
        exit(1);
    }
}

// Guest side: Sync data FROM host (invalidate cache)
void virtgpu_shmem_sync_from_host(virtgpu_shmem * shmem) {
    local_virtgpu_shmem * local_shmem = (local_virtgpu_shmem *)shmem;
    if (!local_shmem || !local_shmem->mmap_ptr) return;

    if (msync(local_shmem->mmap_ptr, local_shmem->mmap_size, MS_INVALIDATE) != 0) {
        printf("FATAL: sync from host failed: %s\n", strerror(errno));
        exit(1);
    }
}

// virtgpu_shmem_create is implemented in virtgpu-common.cpp (interface wrapper)

} // extern "C"
