#include "virtgpu-forward-impl.h"

// CACHE COHERENCY: Helper functions for guest/host coordination (FD + memory mapping)
typedef struct {
    int original_fd;
    void * original_ptr;
    size_t size;
} buffer_cache_coherency_info;

static buffer_cache_coherency_info unmap_buffer_for_host_operation(apir_buffer_context_t * buffer_context) {
    buffer_cache_coherency_info info = {-1, NULL, 0};

    if (!buffer_context->shmem.backend_data) {
        return info; // No backend data available
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)buffer_context->shmem.backend_data;

    // Store original mapping info
    info.original_ptr = buffer_context->shmem.mmap_ptr;
    info.size = buffer_context->shmem.mmap_size;

    // Flush writes before unmapping to ensure host sees them
    if (info.original_ptr && info.size > 0) {
        if (msync(info.original_ptr, info.size, MS_SYNC) != 0) {
            printf("FATAL: msync MS_SYNC failed: %s\n", strerror(errno));
            exit(1);
        }

        // Unmap the buffer
        if (munmap(info.original_ptr, info.size) != 0) {
            printf("FATAL: munmap failed: %s\n", strerror(errno));
            exit(1);
        }
        buffer_context->shmem.mmap_ptr = NULL; // Mark as unmapped
    }

    // Close FD for WSL2/Windows filesystem cache coherency
    if (winapi_buf->fd >= 0) {
        info.original_fd = winapi_buf->fd;
        close(winapi_buf->fd);
        winapi_buf->fd = -1;
    }

    return info;
}

static void remap_buffer_after_host_operation(apir_buffer_context_t * buffer_context,
                                             const buffer_cache_coherency_info * info) {
    if (!buffer_context->shmem.backend_data || info->original_fd < 0) {
        return; // Nothing to reopen
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)buffer_context->shmem.backend_data;

    // Reopen file with fresh FD to see host changes
    int fresh_fd = open(winapi_buf->file_path, O_RDWR);
    if (fresh_fd < 0) {
        printf("FATAL: Failed to reopen file after host operation: %s\n", strerror(errno));
        exit(1);
    }
    winapi_buf->fd = fresh_fd;

    // Remap at original address with fresh FD - MUST succeed at same address
    void * remapped = mmap(info->original_ptr, info->size, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED, fresh_fd, 0);

    if (remapped == MAP_FAILED || remapped != info->original_ptr) {
        printf("FATAL: Failed to remap buffer at original address %p: %s\n",
               info->original_ptr, strerror(errno));
        printf("FATAL: Buffer MUST be mapped at exact same address or tensors will be invalid\n");
        exit(1);
    }

    buffer_context->shmem.mmap_ptr = remapped;

    // Invalidate cache to see host changes
    if (msync(remapped, info->size, MS_INVALIDATE) != 0) {
        printf("FATAL: msync MS_INVALIDATE failed: %s\n", strerror(errno));
        exit(1);
    }
}

void * apir_buffer_get_base(virtgpu * gpu, apir_buffer_context_t * buffer_context) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_GET_BASE);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    uintptr_t base;
    apir_decode_uintptr_t(decoder, &base);

    remote_call_finish(gpu, encoder, decoder);

    return (void *) base;
}

void apir_buffer_set_tensor(virtgpu *               gpu,
                            apir_buffer_context_t * buffer_context,
                            ggml_tensor *           tensor,
                            const void *            data,
                            size_t                  offset,
                            size_t                  size) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_SET_TENSOR);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);
    apir_encode_ggml_tensor(encoder, tensor);

    virtgpu_shmem   temp_shmem;  // Local storage for large buffers
    virtgpu_shmem * shmem = &temp_shmem;
    bool using_shared_shmem = false;

    if (size <= gpu->data_shmem.mmap_size) {
        // Lock mutex before using shared data_shmem buffer
        if (mtx_lock(&gpu->data_shmem_mutex) != thrd_success) {
            GGML_ABORT("Failed to lock data_shmem mutex");
        }
        using_shared_shmem = true;
        shmem = &gpu->data_shmem;

    } else if (virtgpu_shmem_create(gpu, size, shmem)) {
        GGML_ABORT("Couldn't allocate the guest-host shared buffer");
    }

    memcpy(shmem->mmap_ptr, data, size);

    // Cache coherency: flush guest writes so host can see them
    if (msync(shmem->mmap_ptr, size, MS_SYNC) != 0) {
        printf("msync failed: %s\n", strerror(errno));
    }

    apir_encode_virtgpu_shmem_res_id(encoder, shmem->res_id);

    apir_encode_size_t(encoder, &offset);
    apir_encode_size_t(encoder, &size);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    remote_call_finish(gpu, encoder, decoder);

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }

    return;
}

void apir_buffer_get_tensor(virtgpu *               gpu,
                            apir_buffer_context_t * buffer_context,
                            const ggml_tensor *     tensor,
                            void *                  data,
                            size_t                  offset,
                            size_t                  size) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_GET_TENSOR);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);
    apir_encode_ggml_tensor(encoder, tensor);

    virtgpu_shmem   temp_shmem;  // Local storage for large buffers
    virtgpu_shmem * shmem = &temp_shmem;
    bool using_shared_shmem = false;

    if (size <= gpu->data_shmem.mmap_size) {
        // Lock mutex before using shared data_shmem buffer
        if (mtx_lock(&gpu->data_shmem_mutex) != thrd_success) {
            GGML_ABORT("Failed to lock data_shmem mutex");
        }
        using_shared_shmem = true;
        shmem = &gpu->data_shmem;

    } else if (virtgpu_shmem_create(gpu, size, shmem)) {
        GGML_ABORT("Couldn't allocate the guest-host shared buffer");
    }

    apir_encode_virtgpu_shmem_res_id(encoder, shmem->res_id);
    apir_encode_size_t(encoder, &offset);
    apir_encode_size_t(encoder, &size);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    // Cache coherency: invalidate guest cache so it can see host writes
    if (msync(shmem->mmap_ptr, size, MS_INVALIDATE) != 0) {
        printf("msync failed: %s\n", strerror(errno));
    }

    memcpy(data, shmem->mmap_ptr, size);

    remote_call_finish(gpu, encoder, decoder);

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }
}

bool apir_buffer_cpy_tensor(virtgpu *               gpu,
                            apir_buffer_context_t * buffer_context,
                            const ggml_tensor *     src,
                            const ggml_tensor *     dst) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    // CACHE COHERENCY: Unmap buffer and close FD before host copies to dst buffer
    buffer_cache_coherency_info info = unmap_buffer_for_host_operation(buffer_context);

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_CPY_TENSOR);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);
    apir_encode_ggml_tensor(encoder, src);
    apir_encode_ggml_tensor(encoder, dst);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    bool ret_val;
    apir_decode_bool_t(decoder, &ret_val);

    remote_call_finish(gpu, encoder, decoder);

    // CACHE COHERENCY: Remap buffer with fresh FD to see host changes
    remap_buffer_after_host_operation(buffer_context, &info);

    return ret_val;
}

void apir_buffer_clear(virtgpu * gpu, apir_buffer_context_t * buffer_context, uint8_t value) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    // CACHE COHERENCY: Unmap buffer and close FD before host clears buffer
    buffer_cache_coherency_info info = unmap_buffer_for_host_operation(buffer_context);

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_CLEAR);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);
    apir_encode_uint8_t(encoder, &value);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    remote_call_finish(gpu, encoder, decoder);

    // CACHE COHERENCY: Remap buffer with fresh FD to see host changes
    remap_buffer_after_host_operation(buffer_context, &info);
}

void apir_buffer_free_buffer(virtgpu * gpu, apir_buffer_context_t * buffer_context) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_FREE_BUFFER);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    remote_call_finish(gpu, encoder, decoder);
}
