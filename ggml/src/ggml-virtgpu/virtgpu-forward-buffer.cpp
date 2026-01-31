#include "virtgpu-forward-impl.h"

// CACHE COHERENCY: Helper functions for guest/host FD coordination
static int close_buffer_fd_for_host_operation(apir_buffer_context_t * buffer_context) {
    if (!buffer_context->shmem.backend_data) {
        return -1; // No backend data available
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)buffer_context->shmem.backend_data;
    if (winapi_buf->fd >= 0) {
        int original_fd = winapi_buf->fd;
        close(winapi_buf->fd);
        winapi_buf->fd = -1;
        return original_fd;
    }
    return -1;
}

static void reopen_buffer_fd_after_host_operation(apir_buffer_context_t * buffer_context, int original_fd) {
    if (!buffer_context->shmem.backend_data || original_fd < 0) {
        return; // Nothing to reopen
    }

    ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)buffer_context->shmem.backend_data;

    // Reopen file with fresh FD to see host changes
    int fresh_fd = open(winapi_buf->file_path, O_RDWR);
    if (fresh_fd >= 0) {
        winapi_buf->fd = fresh_fd;

        // Invalidate cache to see host changes
        if (buffer_context->shmem.mmap_ptr) {
            if (msync(buffer_context->shmem.mmap_ptr, buffer_context->shmem.mmap_size, MS_INVALIDATE) != 0) {
                printf("Warning: msync MS_INVALIDATE failed: %s\n", strerror(errno));
            }
        }
    } else {
        printf("Warning: Failed to reopen file after host operation: %s\n", strerror(errno));
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

    // CACHE COHERENCY: Close guest FD before host copies to dst buffer
    int original_fd = close_buffer_fd_for_host_operation(buffer_context);

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_CPY_TENSOR);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);
    apir_encode_ggml_tensor(encoder, src);
    apir_encode_ggml_tensor(encoder, dst);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    bool ret_val;
    apir_decode_bool_t(decoder, &ret_val);

    remote_call_finish(gpu, encoder, decoder);

    // CACHE COHERENCY: Reopen fresh FD and invalidate cache to see host changes
    reopen_buffer_fd_after_host_operation(buffer_context, original_fd);

    return ret_val;
}

void apir_buffer_clear(virtgpu * gpu, apir_buffer_context_t * buffer_context, uint8_t value) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    // CACHE COHERENCY: Close guest FD before host clears buffer
    int original_fd = close_buffer_fd_for_host_operation(buffer_context);

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BUFFER_CLEAR);

    apir_encode_apir_buffer_host_handle_t(encoder, &buffer_context->host_handle);
    apir_encode_uint8_t(encoder, &value);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    remote_call_finish(gpu, encoder, decoder);

    // CACHE COHERENCY: Reopen fresh FD and invalidate cache to see host changes
    reopen_buffer_fd_after_host_operation(buffer_context, original_fd);
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
