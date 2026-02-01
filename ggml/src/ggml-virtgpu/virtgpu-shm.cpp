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
#include <sys/stat.h>

#define DEBUG_CACHE_COHERENCY 0

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
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: unmap_for_host called with NULL shmem or backend_data\n");
#endif
        return;
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)local_shmem->backend_data;
#if DEBUG_CACHE_COHERENCY
    printf("DEBUG: unmap_for_host res_id=%u mmap_ptr=%p fd=%d\n", local_shmem->res_id, local_shmem->mmap_ptr, winapi_buf->fd);
#endif

    // Show data before unmapping
    if (local_shmem->mmap_ptr && local_shmem->mmap_size > 0) {
#if DEBUG_CACHE_COHERENCY
        // Show first 8 bytes as debug
        uint32_t data_before_unmap = 0;
        if (local_shmem->mmap_size >= 4) {
            data_before_unmap = *(uint32_t*)local_shmem->mmap_ptr;
        }
        printf("DEBUG: unmap - data_before_unmap=0x%08x\n", data_before_unmap);
#endif
    }

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
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: unmap - closing FD=%d\n", winapi_buf->fd);
#endif
        close(winapi_buf->fd);
        winapi_buf->fd = -1;
    }

#if DEBUG_CACHE_COHERENCY
    printf("DEBUG: unmap_for_host res_id=%u COMPLETED\n", local_shmem->res_id);
#endif
}

// Guest side: Reopen file and remap at original address to see host changes
int virtgpu_shmem_remap_after_host(virtgpu_shmem * shmem, void * original_ptr) {
    local_virtgpu_shmem * local_shmem = (local_virtgpu_shmem *)shmem;
    if (!local_shmem || !local_shmem->backend_data) {
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: remap_after_host called with NULL shmem or backend_data\n");
#endif
        return 0;
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)local_shmem->backend_data;
#if DEBUG_CACHE_COHERENCY
    printf("DEBUG: remap_after_host res_id=%u original_ptr=%p file_path=%s\n",
           local_shmem->res_id, original_ptr, winapi_buf->file_path);
#endif

    // Reopen file with fresh FD to see host changes
    if (winapi_buf->file_path[0]) {
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: Reopening file: %s\n", winapi_buf->file_path);
#endif
        int fresh_fd = open(winapi_buf->file_path, O_RDWR);
        if (fresh_fd < 0) {
            printf("ERROR: Failed to reopen file after host operation: %s (errno=%d)\n", strerror(errno), errno);
            return -1;  // Return error instead of exit
        }
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: Fresh FD=%d opened successfully\n", fresh_fd);
#endif
        // Check file size
        struct stat file_stat;
        if (fstat(fresh_fd, &file_stat) != 0) {
            printf("ERROR: fstat failed: %s\n", strerror(errno));
            close(fresh_fd);
            return -1;  // Return error instead of exit
        }
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: File size=%ld, mmap_size=%zu\n", file_stat.st_size, local_shmem->mmap_size);
#endif
        // Check if file size matches what we want to map
        if ((size_t)file_stat.st_size < local_shmem->mmap_size) {
            printf("ERROR: File too small: file_size=%ld < mmap_size=%zu\n", file_stat.st_size, local_shmem->mmap_size);
            close(fresh_fd);
            return -1;  // Return error instead of exit
        }

        winapi_buf->fd = fresh_fd;
#if DEBUG_CACHE_COHERENCY
        // Debug: Check address alignment
        uintptr_t addr = (uintptr_t)original_ptr;
        printf("DEBUG: Remapping at address=%p (0x%lx), size=%zu, page_aligned=%s\n",
               original_ptr, addr, local_shmem->mmap_size,
               (addr % 4096 == 0) ? "yes" : "no");
#endif
        // Remap at original address - must succeed at same address
        void * remapped = mmap(original_ptr, local_shmem->mmap_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fresh_fd, 0);

        if (remapped == MAP_FAILED || remapped != original_ptr) {
            printf("ERROR: Failed to remap buffer at original address %p: %s (errno=%d)\n",
                   original_ptr, strerror(errno), errno);
            printf("ERROR: remapped=%p, expected=%p\n", remapped, original_ptr);
            close(fresh_fd);
            return -1;  // Return error instead of exit - continue without cache coherency
        }
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: Remap successful at %p\n", remapped);
#endif
        local_shmem->mmap_ptr = remapped;

        // Invalidate cache to see host changes
        if (msync(remapped, local_shmem->mmap_size, MS_INVALIDATE) != 0) {
            printf("ERROR: msync MS_INVALIDATE failed: %s\n", strerror(errno));
            return -1;  // Return error instead of exit
        }
#if DEBUG_CACHE_COHERENCY
        printf("DEBUG: msync MS_INVALIDATE successful\n");

        // Show data after remapping to see if we got host changes
        uint32_t data_after_remap = 0;
        if (local_shmem->mmap_size >= 4) {
            data_after_remap = *(uint32_t*)remapped;
        }
        printf("DEBUG: remap - data_after_remap=0x%08x\n", data_after_remap);
#endif
    }

#if DEBUG_CACHE_COHERENCY
    printf("DEBUG: remap_after_host res_id=%u COMPLETED successfully\n", local_shmem->res_id);
#endif
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
