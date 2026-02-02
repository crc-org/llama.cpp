#include "virtgpu-forward-impl.h"
#include "virtgpu-shm.h"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sys/mman.h>

// Uncomment to enable checksum debugging
#define CHECKSUM_CGRAPH_BUFFERS

// CACHE COHERENCY: External function for guest-side cache coherency
extern "C" void close_specific_session_files_for_guest(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);

// Simple logging control for graph compute messages
#define GRAPH_COMPUTE_DEBUG 0
#define GRAPH_LOG(...) do { if (GRAPH_COMPUTE_DEBUG) printf(__VA_ARGS__); } while(0)

// Simplified cache coherency for Windows shared files

// Simplified cache coherency using unified structure

// Track original pointers for remapping
static std::unordered_map<virtgpu_shmem *, void *> original_ptrs;

#ifdef CHECKSUM_CGRAPH_BUFFERS
// Simple checksum for data verification
static uint32_t simple_checksum(const void * data, size_t size) {
    const uint8_t * bytes = (const uint8_t *)data;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum * 31) + bytes[i];
    }
    return checksum;
}
#endif

// Step 2: Unmap all buffers for host access
static void unmap_graph_buffers_for_host(const std::vector<virtgpu_shmem *> & shmems) {
    for (size_t i = 0; i < shmems.size(); i++) {
        virtgpu_shmem * shmem = shmems[i];
        if (shmem && shmem->backend_data) {  // Only for Windows shared files
            // Compute checksum before unmapping
#ifdef CHECKSUM_CGRAPH_BUFFERS
            if (shmem->mmap_ptr && shmem->mmap_size > 0) {
                uint32_t checksum = simple_checksum(shmem->mmap_ptr, shmem->mmap_size);
                printf("[FRONTEND_UNMAP] Buffer %zu res_id=%u: checksum=0x%08x size=%zu (before unmap)\n",
                       i, shmem->res_id, checksum, shmem->mmap_size);
            }
#endif
            original_ptrs[shmem] = shmem->mmap_ptr;  // Store original pointer
            virtgpu_shmem_unmap_for_host(shmem);
        }
    }
}

// Step 3: Remap all buffers after host processing
static void remap_graph_buffers_after_host(const std::vector<virtgpu_shmem *> & shmems) {
    for (size_t i = 0; i < shmems.size(); i++) {
        virtgpu_shmem * shmem = shmems[i];
        if (shmem && shmem->backend_data && original_ptrs.find(shmem) != original_ptrs.end()) {
            void * original_ptr = original_ptrs[shmem];
            virtgpu_shmem_remap_after_host(shmem, original_ptr);

            // Compute checksum after remapping to see host changes
#ifdef CHECKSUM_CGRAPH_BUFFERS
            if (shmem->mmap_ptr && shmem->mmap_size > 0) {
                uint32_t checksum = simple_checksum(shmem->mmap_ptr, shmem->mmap_size);
                printf("[FRONTEND_REMAP] Buffer %zu res_id=%u: checksum=0x%08x size=%zu (after remap)\n",
                       i, shmem->res_id, checksum, shmem->mmap_size);
            }
#endif
        }
    }
    original_ptrs.clear();  // Clean up tracking map
}

static long long current_time_ms() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // Use CLOCK_MONOTONIC for elapsed time
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Step 1: Find all unique buffer shmems involved in the graph (sorted for consistent ordering)
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

    // Convert to vector and sort by buffer size for consistent ordering with host
    std::vector<virtgpu_shmem *> result(shmem_set.begin(), shmem_set.end());
    std::sort(result.begin(), result.end(), [](const virtgpu_shmem * a, const virtgpu_shmem * b) {
        return a->mmap_size < b->mmap_size;
    });
    return result;
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

#ifdef CHECKSUM_CGRAPH_BUFFERS
    printf("GUEST #%u: Processing cgraph with %d nodes, %zu buffers\n",
           guest_graph_compute_counter, cgraph->n_nodes, buffer_shmems.size());

    // Expected checksums from CPU backend (reference values)
    static const uint32_t expected_checksums[] = {0x00000000, 0x20cf3e51, 0x8993d3dc};
    static const size_t num_expected = sizeof(expected_checksums) / sizeof(expected_checksums[0]);

#if 0
    // Calculate checksums before processing (buffer level) - DISABLED
    printf("GUEST #%u: Buffer checksums before processing:\n", guest_graph_compute_counter);
    for (size_t i = 0; i < buffer_shmems.size(); i++) {
        virtgpu_shmem * shmem = buffer_shmems[i];
        if (shmem && shmem->mmap_ptr && shmem->mmap_size > 0) {
            uint32_t checksum = simple_checksum(shmem->mmap_ptr, shmem->mmap_size);

            // Compare against expected values
            if (i < num_expected) {
                if (checksum == expected_checksums[i]) {
                    printf("GUEST #%u: Buffer %zu checksum before: 0x%08x ✓ (matches CPU)\n",
                           guest_graph_compute_counter, i, checksum);
                } else {
                    printf("GUEST #%u: Buffer %zu checksum before: 0x%08x ✗ (expected 0x%08x)\n",
                           guest_graph_compute_counter, i, checksum, expected_checksums[i]);
                    printf("FATAL: Buffer %zu has wrong data - model weights not loaded properly!\n", i);
                    _exit(1);
                }
            } else {
                printf("GUEST #%u: Buffer %zu checksum before: 0x%08x (no reference)\n",
                       guest_graph_compute_counter, i, checksum);
            }
        }
    }
#endif
#endif


    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_BACKEND_GRAPH_COMPUTE);

    std::vector<uint8_t> cgraph_data;
    size_t               cgraph_size = apir_serialize_ggml_cgraph(cgraph, cgraph_data);

    virtgpu_shmem   temp_shmem;  // Local storage for large buffers
    virtgpu_shmem * shmem = &temp_shmem;
    bool using_shared_shmem = false;

    if (cgraph_size <= gpu->data_shmem.mmap_size) {
        // Lock mutex before using shared data_shmem buffer
        if (mtx_lock(&gpu->data_shmem_mutex) != thrd_success) {
            printf("FATAL: Failed to lock data_shmem mutex\n");
            exit(1);
        }
        using_shared_shmem = true;
        shmem = &gpu->data_shmem;
    } else if (virtgpu_shmem_create(gpu, cgraph_size, shmem)) {
        printf("FATAL: Couldn't allocate the guest-host shared buffer\n");
        exit(1);
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

#ifdef CHECKSUM_CGRAPH_BUFFERS
    // Calculate checksums after processing (buffer level)
    printf("GUEST #%u: Buffer checksums after processing:\n", guest_graph_compute_counter);
    for (size_t i = 0; i < buffer_shmems.size(); i++) {
        virtgpu_shmem * shmem = buffer_shmems[i];
        if (shmem && shmem->mmap_ptr && shmem->mmap_size > 0) {
            uint32_t checksum = simple_checksum(shmem->mmap_ptr, shmem->mmap_size);
            printf("GUEST #%u: Buffer %zu checksum after:  0x%08x\n",
                   guest_graph_compute_counter, i, checksum);
        }
    }
#endif

    // Unlock mutex before cleanup
    if (using_shared_shmem) {
        mtx_unlock(&gpu->data_shmem_mutex);
    } else {
        virtgpu_shmem_destroy(gpu, shmem);
    }

    return status;
}
