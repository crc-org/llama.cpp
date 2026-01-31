#include "virtgpu-forward-impl.h"
#include <unordered_set>
#include <unordered_map>

// CACHE COHERENCY: External function for guest-side cache coherency
extern "C" void close_specific_session_files_for_guest(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);

// Simple logging control for graph compute messages
#define GRAPH_COMPUTE_DEBUG 0
#define GRAPH_LOG(...) do { if (GRAPH_COMPUTE_DEBUG) printf(__VA_ARGS__); } while(0)

// Buffer mapping info for cache coherency operations
struct buffer_mapping_info {
    void * original_ptr;
    size_t size;
    uint32_t res_id;
};

// CACHE COHERENCY: Helper functions for WSL2/Windows shared file coordination
static void close_graph_buffer_fds_for_host(const std::unordered_set<apir_buffer_context_t *> & buffer_contexts,
                                            std::unordered_map<apir_buffer_context_t *, int> & original_fds) {
    for (apir_buffer_context_t * ctx : buffer_contexts) {
        if (ctx && ctx->shmem.backend_data) {
            ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)ctx->shmem.backend_data;
            if (winapi_buf->fd >= 0) {
                original_fds[ctx] = winapi_buf->fd;
                close(winapi_buf->fd);
                winapi_buf->fd = -1;
                GRAPH_LOG("[GRAPH_COMPUTE] Closed FD %d for buffer %p to flush cache\n", original_fds[ctx], ctx->shmem.mmap_ptr);
            }
        }
    }
}

static void reopen_graph_buffer_fds_after_host(const std::unordered_map<apir_buffer_context_t *, int> & original_fds) {
    for (const auto & pair : original_fds) {
        apir_buffer_context_t * ctx = pair.first;
        int original_fd = pair.second;

        if (ctx && ctx->shmem.backend_data && original_fd >= 0) {
            ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)ctx->shmem.backend_data;

            // Reopen file with fresh FD to see Windows host changes
            int fresh_fd = open(winapi_buf->file_path, O_RDWR);
            if (fresh_fd >= 0) {
                winapi_buf->fd = fresh_fd;
                GRAPH_LOG("[GRAPH_COMPUTE] Reopened file %s with fresh FD %d\n", winapi_buf->file_path, fresh_fd);

                // Invalidate cache to see host changes
                if (ctx->shmem.mmap_ptr) {
                    if (msync(ctx->shmem.mmap_ptr, ctx->shmem.mmap_size, MS_INVALIDATE) != 0) {
                        printf("FATAL: msync MS_INVALIDATE failed after remap: %s\n", strerror(errno));
                        exit(1);
                    }
                }
            } else {
                printf("FATAL: Failed to reopen file %s: %s\n", winapi_buf->file_path, strerror(errno));
                exit(1);
            }
        }
    }
}

static long long current_time_ms() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // Use CLOCK_MONOTONIC for elapsed time
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Step 1: Find all unique buffer contexts involved in the graph
static std::unordered_set<apir_buffer_context_t *> find_graph_buffer_contexts(ggml_cgraph * cgraph) {
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

    return buffer_contexts;
}

// Step 2: Store original mapping info and unmap buffers
static void unmap_graph_buffers_for_host(const std::unordered_set<apir_buffer_context_t *> & buffer_contexts,
                                          std::unordered_map<apir_buffer_context_t *, buffer_mapping_info> & original_mappings,
                                          std::unordered_map<apir_buffer_context_t *, int> & original_fds) {
    // CACHE COHERENCY: Close all FDs before unmapping (only for real buffer contexts)
    close_graph_buffer_fds_for_host(buffer_contexts, original_fds);

    for (apir_buffer_context_t * ctx : buffer_contexts) {
        if (ctx && ctx->shmem.mmap_ptr) {
            buffer_mapping_info info;
            info.original_ptr = ctx->shmem.mmap_ptr;
            info.size = ctx->shmem.mmap_size;
            info.res_id = ctx->shmem.res_id;

            original_mappings[ctx] = info;

            GRAPH_LOG("[GRAPH_COMPUTE] Unmapping buffer %p (size=%zu, res_id=%u)\n",
                       info.original_ptr, info.size, info.res_id);

            // Flush writes before unmapping to ensure host sees them
            if (msync(info.original_ptr, info.size, MS_SYNC) != 0) {
                printf("FATAL: msync MS_SYNC failed for buffer: %s\n", strerror(errno));
                exit(1);
            }

            // Unmap the buffer - for temp shmem (virtgpu_shmem), this is sufficient
            // Real buffer contexts with backend_data will be handled by FD close/reopen
            if (munmap(info.original_ptr, info.size) != 0) {
                printf("FATAL: munmap failed for buffer %p: %s\n", info.original_ptr, strerror(errno));
                exit(1);
            }
            ctx->shmem.mmap_ptr = NULL; // Mark as unmapped
            GRAPH_LOG("[GRAPH_COMPUTE] Successfully unmapped buffer %p\n", info.original_ptr);
        }
    }
}

// Step 4: Remap all unmapped buffers back to their original addresses
static void remap_graph_buffers_after_host(virtgpu * gpu,
                                            const std::unordered_map<apir_buffer_context_t *, buffer_mapping_info> & original_mappings,
                                            const std::unordered_map<apir_buffer_context_t *, int> & original_fds) {
    UNUSED(gpu);

    // CACHE COHERENCY: Reopen FDs first to get fresh file handles
    reopen_graph_buffer_fds_after_host(original_fds);

    for (auto & pair : original_mappings) {
        apir_buffer_context_t * ctx = pair.first;
        const buffer_mapping_info & info = pair.second;

        if (ctx && ctx->shmem.mmap_ptr == NULL) { // Was unmapped
            GRAPH_LOG("[GRAPH_COMPUTE] Remapping buffer at %p (size=%zu, res_id=%u)\n",
                       info.original_ptr, info.size, info.res_id);

            void * remapped = NULL;

            if (ctx->shmem.backend_data) {
                // Real buffer context with Windows shared file - use fresh FD
                ggml_winapi_shared_buffer_t * winapi_buf = (ggml_winapi_shared_buffer_t *)ctx->shmem.backend_data;
                int fresh_fd = winapi_buf->fd;

                // Remap at original address using fresh FD - MUST succeed at same address
                remapped = mmap(info.original_ptr, info.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_FIXED, fresh_fd, 0);

                if (remapped == MAP_FAILED || remapped != info.original_ptr) {
                    printf("FATAL: Failed to remap buffer at original address %p: %s\n",
                           info.original_ptr, strerror(errno));
                    printf("FATAL: Buffer MUST be mapped at exact same address or tensors will be invalid\n");
                    exit(1);
                }
            } else {
                printf("FATAL: Cannot remap buffer without backend_data (Windows shared file info)\n");
                exit(1);
            }

            ctx->shmem.mmap_ptr = remapped;
            GRAPH_LOG("[GRAPH_COMPUTE] Successfully remapped buffer to %p\n", remapped);
        }
    }
}

ggml_status apir_backend_graph_compute(virtgpu * gpu, ggml_cgraph * cgraph) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    static uint32_t guest_graph_compute_counter = 0;
    guest_graph_compute_counter++;

    printf("[DEBUG] GUEST #%u: === apir_backend_graph_compute ENTRY === gpu=%p cgraph=%p\n",
           guest_graph_compute_counter, gpu, cgraph);
    printf("[DEBUG] GUEST #%u: Input cgraph has %d nodes\n",
           guest_graph_compute_counter, cgraph ? cgraph->n_nodes : -1);

    // Step 1: Find all unique buffer contexts involved in the graph
    std::unordered_set<apir_buffer_context_t *> buffer_contexts = find_graph_buffer_contexts(cgraph);

    printf("[DEBUG] GUEST #%u: Found %zu unique buffer contexts\n",
           guest_graph_compute_counter, buffer_contexts.size());

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

    // For temp shmem (when not using shared memory), we need separate cache coherency
    // since it doesn't have Windows shared file backing like regular buffers

    // CACHE COHERENCY: If using shared memory, unmap the secondary command buffer
    if (using_shared_shmem) {
        uint64_t shmem_buffer_handle = shmem->res_id;
        close_specific_session_files_for_guest(0, &shmem_buffer_handle, 1);
        printf("[DEBUG] GUEST: Unmapped shared data_shmem buffer res_id=%u for host access\n", shmem->res_id);
    }

    // Step 2: Store original mapping info and unmap buffers (just before host access)
    std::unordered_map<apir_buffer_context_t *, buffer_mapping_info> original_mappings;
    std::unordered_map<apir_buffer_context_t *, int> original_fds;
    unmap_graph_buffers_for_host(buffer_contexts, original_mappings, original_fds);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    ggml_status status = GGML_STATUS_ABORTED;
    apir_decode_ggml_status(decoder, &status);

    remote_call_finish(gpu, encoder, decoder);

    // Step 4: Remap all unmapped buffers back to their original addresses
    remap_graph_buffers_after_host(gpu, original_mappings, original_fds);

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }

    return status;
}
