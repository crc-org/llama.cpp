#include "virtgpu-forward-impl.h"
#include <unordered_set>
#include <unordered_map>

// Simple logging control for graph compute messages
#define GRAPH_COMPUTE_DEBUG 0
#define GRAPH_LOG(...) do { if (GRAPH_COMPUTE_DEBUG) printf(__VA_ARGS__); } while(0)

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
    for (uint32_t i = 0; i < (uint32_t)cgraph->n_nodes; i++) {
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

    // Step 2: Store original mapping info and unmap buffers
    struct buffer_mapping_info {
        void * original_ptr;
        size_t size;
        uint32_t res_id;
        int fd;  // File descriptor for remapping
    };
    std::unordered_map<apir_buffer_context_t *, buffer_mapping_info> original_mappings;

    for (apir_buffer_context_t * ctx : buffer_contexts) {
        if (ctx && ctx->shmem.mmap_ptr) {
            buffer_mapping_info info;
            info.original_ptr = ctx->shmem.mmap_ptr;
            info.size = ctx->shmem.mmap_size;
            info.res_id = ctx->shmem.res_id;

            // For Windows: get fd from the winapi shared buffer in backend_data
            if (ctx->shmem.backend_data) {
                ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)ctx->shmem.backend_data;
                info.fd = winapi_buf->fd;
            } else {
                printf("Warning: Windows buffer has no backend_data, skipping unmap\n");
                continue;
            }

            original_mappings[ctx] = info;

            GRAPH_LOG("[GRAPH_COMPUTE] Unmapping buffer %p (size=%zu, res_id=%u, fd=%d)\n",
                       info.original_ptr, info.size, info.res_id, info.fd);

            // Flush writes before unmapping to ensure host sees them
            if (msync(info.original_ptr, info.size, MS_SYNC) != 0) {
                printf("msync MS_SYNC failed for buffer: %s\n", strerror(errno));
            }

            // Unmap the buffer
            if (munmap(info.original_ptr, info.size) != 0) {
                printf("munmap failed for buffer %p: %s\n", info.original_ptr, strerror(errno));
            } else {
                ctx->shmem.mmap_ptr = NULL; // Mark as unmapped
                GRAPH_LOG("[GRAPH_COMPUTE] Successfully unmapped buffer %p\n", info.original_ptr);
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

    // Step 4: Remap all unmapped buffers back to their original addresses
    for (auto & pair : original_mappings) {
        apir_buffer_context_t * ctx = pair.first;
        buffer_mapping_info & info = pair.second;

        if (ctx && ctx->shmem.mmap_ptr == NULL) { // Was unmapped
            GRAPH_LOG("[GRAPH_COMPUTE] Remapping buffer at %p (size=%zu, res_id=%u, fd=%d)\n",
                       info.original_ptr, info.size, info.res_id, info.fd);

            void * remapped = NULL;

            // Try to remap at the original address using the stored fd
            // For temporary files, offset should be 0
            uint64_t offset = 0;
            remapped = mmap(info.original_ptr, info.size, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, info.fd, offset);

            if (remapped == MAP_FAILED || remapped != info.original_ptr) {
                printf("Failed to remap buffer at original address %p: %s\n",
                       info.original_ptr, strerror(errno));

                // Fallback: Try without MAP_FIXED (let system choose address)
                remapped = mmap(NULL, info.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, info.fd, offset);

                if (remapped == MAP_FAILED) {
                    printf("FATAL: Failed to remap buffer: %s\n", strerror(errno));
                    exit(1);
                }

                if (remapped != info.original_ptr) {
                    printf("Buffer remapped to new address %p (was %p)\n", remapped, info.original_ptr);
                }
            }

            ctx->shmem.mmap_ptr = remapped;
            GRAPH_LOG("[GRAPH_COMPUTE] Successfully remapped buffer to %p\n", remapped);

            // Invalidate cache to see any host changes
            if (msync(remapped, info.size, MS_INVALIDATE) != 0) {
                printf("msync MS_INVALIDATE failed after remap: %s\n", strerror(errno));
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
