#include "virtgpu-forward-impl.h"
#include "virtgpu-shm.h"
#include <unordered_set>
#include <unordered_map>
#include <vector>

// CACHE COHERENCY: External function for guest-side cache coherency
extern "C" void close_specific_session_files_for_guest(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);

// Simple logging control for graph compute messages
#define GRAPH_COMPUTE_DEBUG 0
#define GRAPH_LOG(...) do { if (GRAPH_COMPUTE_DEBUG) printf(__VA_ARGS__); } while(0)

// Simplified cache coherency for Windows shared files

// Simplified cache coherency using unified structure

// Track original pointers for remapping
static std::unordered_map<virtgpu_shmem *, void *> original_ptrs;

// Step 2: Unmap all buffers for host access
static void unmap_graph_buffers_for_host(const std::vector<virtgpu_shmem *> & shmems) {
    for (virtgpu_shmem * shmem : shmems) {
        if (shmem && shmem->backend_data) {  // Only for Windows shared files
            original_ptrs[shmem] = shmem->mmap_ptr;  // Store original pointer
            virtgpu_shmem_unmap_for_host(shmem);
        }
    }
}

// Step 3: Remap all buffers after host processing
static void remap_graph_buffers_after_host(const std::vector<virtgpu_shmem *> & shmems) {
    for (virtgpu_shmem * shmem : shmems) {
        if (shmem && shmem->backend_data && original_ptrs.find(shmem) != original_ptrs.end()) {
            void * original_ptr = original_ptrs[shmem];
            virtgpu_shmem_remap_after_host(shmem, original_ptr);
        }
    }
    original_ptrs.clear();  // Clean up tracking map
}

static long long current_time_ms() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // Use CLOCK_MONOTONIC for elapsed time
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Step 1: Find all unique buffer shmems involved in the graph
static std::vector<virtgpu_shmem *> find_graph_buffer_shmems(ggml_cgraph * cgraph) {
    std::unordered_set<virtgpu_shmem *> shmem_set;

    for (uint32_t i = 0; i < (uint32_t)cgraph->n_nodes; i++) {
        ggml_tensor * node = cgraph->nodes[i];
        if (node->buffer) {
            apir_buffer_context_t * ctx = BUFFER_TO_APIR_CONTEXT(node->buffer);
            shmem_set.insert(&ctx->shmem);
        }
        // Also check source tensors
        for (int j = 0; j < GGML_MAX_SRC; j++) {
            if (node->src[j] && node->src[j]->buffer) {
                apir_buffer_context_t * ctx = BUFFER_TO_APIR_CONTEXT(node->src[j]->buffer);
                shmem_set.insert(&ctx->shmem);
            }
        }
        if (node->view_src && node->view_src->buffer) {
            apir_buffer_context_t * ctx = BUFFER_TO_APIR_CONTEXT(node->view_src->buffer);
            shmem_set.insert(&ctx->shmem);
        }
    }

    return std::vector<virtgpu_shmem *>(shmem_set.begin(), shmem_set.end());
}

// These functions are replaced by the simplified versions above

ggml_status apir_backend_graph_compute(virtgpu * gpu, ggml_cgraph * cgraph) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    static uint32_t guest_graph_compute_counter = 0;
    guest_graph_compute_counter++;


    // Step 1: Find all unique buffer shmems involved in the graph
    std::vector<virtgpu_shmem *> buffer_shmems = find_graph_buffer_shmems(cgraph);

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

    // CACHE COHERENCY: Flush data to ensure host can see it - MANDATORY for guest->host consistency
    if (msync(shmem->mmap_ptr, cgraph_size, MS_SYNC) != 0) {
        printf("FATAL: Failed to sync secondary command buffer: %s\n", strerror(errno));
        exit(1);
    }
    // Step 2: Unmap all buffers for host access (WSL2/Windows cache coherency)
    unmap_graph_buffers_for_host(buffer_shmems);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    ggml_status status = GGML_STATUS_ABORTED;
    apir_decode_ggml_status(decoder, &status);

    remote_call_finish(gpu, encoder, decoder);

    // Step 3: Remap all buffers after host processing (see host changes)
    remap_graph_buffers_after_host(buffer_shmems);

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }

    return status;
}
