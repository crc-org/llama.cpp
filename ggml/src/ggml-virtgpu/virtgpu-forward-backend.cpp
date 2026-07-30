#include "virtgpu-forward-impl.h"

static long long current_time_ms() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // Use CLOCK_MONOTONIC for elapsed time
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int apir_backend_initialize(virtgpu *   gpu,
                            void *      ggml_backend_reg_fct_p,
                            uintptr_t * out_device_handle,
                            uint32_t *  out_backend_id) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BACKEND_INITIALIZE);

    // Send the backend registration function pointer
    uintptr_t function_ptr = (uintptr_t) ggml_backend_reg_fct_p;
    apir_encode_uintptr_t(encoder, &function_ptr);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    uintptr_t device_handle = 0;
    uint32_t  backend_id    = 0;

    GGML_LOG_INFO(GGML_VIRTGPU "%s: Backend initialization returned: %d\n", __func__, ret);

    // Check if backend function succeeded (ret == 0) or failed (ret != 0)
    if (ret == 0) {
        // Success - decode the response
        apir_decode_uintptr_t(decoder, &device_handle);
        apir_decode_uint32_t(decoder, &backend_id);
        GGML_LOG_INFO(GGML_VIRTGPU "%s: Success - device_handle=%p, backend_id=%u\n", __func__, (void *) device_handle,
                      backend_id);
    } else {
        GGML_LOG_ERROR(GGML_VIRTGPU "%s: Backend initialization failed with ret=%d\n", __func__, ret);
    }

    remote_call_finish(gpu, encoder, decoder);

    // Set output parameters
    if (out_device_handle) {
        *out_device_handle = device_handle;
    }

    if (out_backend_id) {
        *out_backend_id = backend_id;
    }

    // Return the result code
    return (ret == 0) ? APIR_BACKEND_INITIALIZE_SUCCESS : APIR_BACKEND_INITIALIZE_BACKEND_INIT_FAILED;
}

ggml_status apir_backend_graph_compute(virtgpu *     gpu,
                                       uintptr_t     device_handle,
                                       uint32_t      backend_id,
                                       ggml_cgraph * cgraph) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BACKEND_GRAPH_COMPUTE);

    std::vector<uint8_t> cgraph_data;
    size_t               cgraph_size = apir_serialize_ggml_cgraph(cgraph, cgraph_data);

    virtgpu_shmem   temp_shmem;  // Local storage for large buffers
    virtgpu_shmem * shmem              = &temp_shmem;
    bool            using_shared_shmem = false;

    if (cgraph_size <= gpu->data_shmem.mmap_size) {
        // Lock mutex before using shared data_shmem buffer
        if (mtx_lock(&gpu->data_shmem_mutex) != thrd_success) {
            GGML_ABORT(GGML_VIRTGPU "%s: Failed to lock data_shmem mutex", __func__);
        }
        using_shared_shmem = true;
        shmem              = &gpu->data_shmem;
    } else if (virtgpu_shmem_create(gpu, cgraph_size, shmem)) {
        GGML_ABORT(GGML_VIRTGPU "%s: Couldn't allocate the guest-host shared buffer", __func__);
    }

    apir_encode_virtgpu_shmem_res_id(encoder, shmem->res_id);

    apir_encode_size_t(encoder, &cgraph_size);

    // Send device handle and backend ID separately
    apir_encode_uintptr_t(encoder, &device_handle);
    apir_encode_uint32_t(encoder, &backend_id);

    char *       shmem_data    = (char *) shmem->mmap_ptr;
    apir_encoder secondary_enc = apir_new_encoder(shmem_data, cgraph_size);

    apir_encode_cgraph_data(&secondary_enc, cgraph_data);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    ggml_status status = GGML_STATUS_ABORTED;
    apir_decode_ggml_status(decoder, &status);

    remote_call_finish(gpu, encoder, decoder);

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }

    return status;
}

void apir_backend_cleanup(virtgpu * gpu, uintptr_t device_handle, uint32_t backend_id) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BACKEND_CLEANUP);

    // Send device handle and backend ID separately
    apir_encode_uintptr_t(encoder, &device_handle);
    apir_encode_uint32_t(encoder, &backend_id);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    remote_call_finish(gpu, encoder, decoder);
}
