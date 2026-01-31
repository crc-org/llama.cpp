#include "virtgpu-forward-impl.h"
#include <unordered_set>

static long long current_time_ms() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // Use CLOCK_MONOTONIC for elapsed time
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

ggml_status apir_backend_graph_compute(virtgpu * gpu, ggml_cgraph * cgraph) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    // Step 1: Find all unique buffer contexts involved in the graph
    std::unordered_set<apir_buffer_context_t *> buffer_contexts;
    for (uint32_t i = 0; i < cgraph->n_nodes; i++) {
        ggml_tensor * node = cgraph->nodes[i];
        if (node->buffer) {
            apir_buffer_context_t * ctx = BUFFER_TO_APIR_CONTEXT(node->buffer);
            buffer_contexts.insert(ctx);
        }
        // Also check source tensors
        for (int j = 0; j < GGML_MAX_SRC; j++) {
            if (node->src[j] && node->src[j]->buffer) {
                apir_buffer_context_t * ctx = BUFFER_TO_APIR_CONTEXT(node->src[j]->buffer);
                buffer_contexts.insert(ctx);
            }
        }
        if (node->view_src && node->view_src->buffer) {
            apir_buffer_context_t * ctx = BUFFER_TO_APIR_CONTEXT(node->view_src->buffer);
            buffer_contexts.insert(ctx);
        }
    }

    // Step 2: Flush buffer data to ensure cache coherency
    // For now, we'll use msync to flush writes without unmapping
    // TODO: Implement unmap/remap strategy once we understand the Windows buffer system better

    for (apir_buffer_context_t * ctx : buffer_contexts) {
        if (ctx && ctx->shmem.mmap_ptr) {
            printf("[GRAPH_COMPUTE] Flushing buffer %p (size=%zu)\n",
                   ctx->shmem.mmap_ptr, ctx->shmem.mmap_size);

            // Flush writes to ensure host sees them
            if (msync(ctx->shmem.mmap_ptr, ctx->shmem.mmap_size, MS_SYNC) != 0) {
                printf("msync failed for buffer: %s\n", strerror(errno));
            }
        }
    }

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BACKEND_GRAPH_COMPUTE);

    std::vector<uint8_t> cgraph_data;
    size_t               cgraph_size = apir_serialize_ggml_cgraph(cgraph, cgraph_data);

    virtgpu_shmem   temp_shmem;  // Local storage for large buffers
    virtgpu_shmem * shmem = &temp_shmem;
    bool using_shared_shmem = false;

    if (cgraph_size <= gpu->data_shmem.mmap_size) {
        // Lock mutex before using shared data_shmem buffer
        if (mtx_lock(&gpu->data_shmem_mutex) != thrd_success) {
            GGML_ABORT("Failed to lock data_shmem mutex");
        }
        using_shared_shmem = true;
        shmem = &gpu->data_shmem;
    } else if (virtgpu_shmem_create(gpu, cgraph_size, shmem)) {
        GGML_ABORT("Couldn't allocate the guest-host shared buffer");
    }

    apir_encode_virtgpu_shmem_res_id(encoder, shmem->res_id);

    apir_encode_size_t(encoder, &cgraph_size);

    char *       shmem_data    = (char *) shmem->mmap_ptr;
    apir_encoder secondary_enc = apir_new_encoder(shmem_data, cgraph_size);

    apir_encode_cgraph_data(&secondary_enc, cgraph_data);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    ggml_status status = GGML_STATUS_ABORTED;
    apir_decode_ggml_status(decoder, &status);

    remote_call_finish(gpu, encoder, decoder);

    // Step 4: Post-compute invalidation to see host changes
    // Invalidate guest cache to see any host writes to the buffers
    for (apir_buffer_context_t * ctx : buffer_contexts) {
        if (ctx && ctx->shmem.mmap_ptr) {
            printf("[GRAPH_COMPUTE] Invalidating buffer %p cache\n", ctx->shmem.mmap_ptr);

            if (msync(ctx->shmem.mmap_ptr, ctx->shmem.mmap_size, MS_INVALIDATE) != 0) {
                printf("msync MS_INVALIDATE failed for buffer: %s\n", strerror(errno));
            }
        }
    }

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }

    return status;
}
