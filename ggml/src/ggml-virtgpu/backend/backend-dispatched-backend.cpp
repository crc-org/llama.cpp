#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"
#include "ggml.h"
#include "shared/apir_backend.h"

#include <cstdint>
#include <unordered_set>
#include <vector>
#include <algorithm>

// Enable checksum debugging for cache coherency verification
//#define CHECKSUM_CGRAPH_BUFFERS

// CACHE COHERENCY: External functions for buffer management (defined in main.cpp)
extern "C" void unmap_all_session_buffers(uint32_t session_id);
extern "C" void ensure_all_session_buffers_mapped(uint32_t session_id);
extern "C" void close_and_reopen_specific_session_files(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);
extern "C" void close_specific_session_files_for_guest(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);
extern "C" void flush_specific_session_buffers(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);

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

uint32_t backend_backend_graph_compute(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(enc);

    static bool async_backend_initialized = false;
    static bool async_backend;
    static uint32_t host_graph_compute_counter = 0;

    host_graph_compute_counter++;

    if (!async_backend_initialized) {
        ggml_backend_dev_props props;

        dev->iface.get_props(dev, &props);
        async_backend             = props.caps.async;
        async_backend_initialized = true;
    }

    uint32_t shmem_res_id;
    apir_decode_virtgpu_shmem_res_id(dec, &shmem_res_id);

    const void * shmem_data = ctx->iface->get_shmem_ptr(ctx->ctx_id, shmem_res_id);
    if (!shmem_data) {
        GGML_LOG_ERROR("Couldn't get the shmem addr from virgl\n");
        apir_decoder_set_fatal(dec);
        return 1;
    }
    size_t cgraph_size;
    apir_decode_size_t(dec, &cgraph_size);

    apir_decoder secondary_dec = apir_new_decoder((const char *) shmem_data, cgraph_size);

    ggml_cgraph * cgraph = apir_decode_ggml_cgraph(&secondary_dec, cgraph_size);

    // SAFETY CHECK: If there are 0 nodes, something went wrong in transmission/serialization
    if (!cgraph || cgraph->n_nodes == 0) {
        printf("ERROR: HOST #%u: Received cgraph with 0 nodes - transmission/serialization failed\n",
               host_graph_compute_counter);
        return 1;
    }
#ifdef CHECKSUM_CGRAPH_BUFFERS
    printf("HOST #%u: Processing cgraph with %d nodes\n",
           host_graph_compute_counter, cgraph->n_nodes);
#endif

    // Extract unique buffer res_ids from decoded graph tensors for cache coherency
    std::unordered_set<uint64_t> buffer_res_ids_set;

    // Process all nodes in the graph
    for (int i = 0; i < cgraph->n_nodes; i++) {
        ggml_tensor * node = cgraph->nodes[i];
        if (node->buffer) {
            // Convert ggml_backend_buffer_t to res_id
            uint32_t res_id = apir_get_buffer_res_id(node->buffer);
            if (res_id != 0) {
                buffer_res_ids_set.insert(res_id);
            }
        }

        // Also check source tensors
        for (int j = 0; j < GGML_MAX_SRC && node->src[j]; j++) {
            if (node->src[j]->buffer) {
                uint32_t res_id = apir_get_buffer_res_id(node->src[j]->buffer);
                if (res_id != 0) {
                    buffer_res_ids_set.insert(res_id);
                }
            }
        }

        // Check view source
        if (node->view_src && node->view_src->buffer) {
            uint32_t res_id = apir_get_buffer_res_id(node->view_src->buffer);
            if (res_id != 0) {
                buffer_res_ids_set.insert(res_id);
            }
        }
    }

    // Convert to array for passing to C functions
    std::vector<uint64_t> buffer_res_ids(buffer_res_ids_set.begin(), buffer_res_ids_set.end());

    // printf("[CACHE] Processing %zu buffers\n", buffer_res_ids.size());

    ggml_status status;
#if APIR_BACKEND_CHECK_SUPPORTS_OP == 1
    for (int idx = 0; idx < cgraph->n_nodes; idx++) {
        ggml_tensor * op = ggml_graph_node(cgraph, idx);
        if (dev->iface.supports_op(dev, op)) {
            continue;
        }
        GGML_LOG_ERROR("Graph node %d (%s) not supported by the backend\n", idx, ggml_op_desc(op));

        status = GGML_STATUS_ABORTED;
        apir_encode_ggml_status(enc, &status);

        return 0;
    }
#endif

    // CACHE COHERENCY: Flush all buffers before computation so host sees guest writes
    flush_specific_session_buffers(ctx->ctx_id, buffer_res_ids.data(), buffer_res_ids.size());

#ifdef CHECKSUM_CGRAPH_BUFFERS
    // Extract unique buffers, sorted (needed for checksums)
    std::unordered_set<ggml_backend_buffer_t> buffer_set;
    for (int i = 0; i < cgraph->n_nodes; i++) {
        ggml_tensor * node = cgraph->nodes[i];
        if (node->buffer) {
            buffer_set.insert(node->buffer);
        }
        // Also check source tensors
        for (int j = 0; j < GGML_MAX_SRC && node->src[j]; j++) {
            if (node->src[j]->buffer) {
                buffer_set.insert(node->src[j]->buffer);
            }
        }
        if (node->view_src && node->view_src->buffer) {
            buffer_set.insert(node->view_src->buffer);
        }
    }

    // Convert to vector and sort by buffer size for consistent ordering with guest
    std::vector<ggml_backend_buffer_t> unique_buffers(buffer_set.begin(), buffer_set.end());
    std::sort(unique_buffers.begin(), unique_buffers.end(), [](const ggml_backend_buffer_t a, const ggml_backend_buffer_t b) {
        return ggml_backend_buffer_get_size(a) < ggml_backend_buffer_get_size(b);
    });

    // Calculate checksums before computation
    printf("[HOST_BEFORE] Buffers before computation:\n");

    for (size_t buffer_index = 0; buffer_index < unique_buffers.size(); buffer_index++) {
        ggml_backend_buffer_t buffer = unique_buffers[buffer_index];

        void * buffer_data = ggml_backend_buffer_get_base(buffer);
        size_t buffer_size = ggml_backend_buffer_get_size(buffer);

        if (buffer_data && buffer_size > 0) {
            uint32_t checksum = simple_checksum(buffer_data, buffer_size);
            uint32_t res_id = apir_get_buffer_res_id(buffer);
            printf("[HOST_BEFORE] res_id=%u: checksum=0x%08x size=%zu\n",
                   res_id, checksum, buffer_size);
        }
    }
#endif

    // Run the actual computation
    status = bck->iface.graph_compute(bck, cgraph);

    if (async_backend) {
        bck->iface.synchronize(bck);
    }

#ifdef CHECKSUM_CGRAPH_BUFFERS
    // Calculate checksums after computation (same sorted buffers)
    printf("\n[HOST_AFTER] Buffers after computation:\n");
    for (size_t buffer_index = 0; buffer_index < unique_buffers.size(); buffer_index++) {
        ggml_backend_buffer_t buffer = unique_buffers[buffer_index];

        void * buffer_data = ggml_backend_buffer_get_base(buffer);
        size_t buffer_size = ggml_backend_buffer_get_size(buffer);

        if (buffer_data && buffer_size > 0) {
            uint32_t checksum = simple_checksum(buffer_data, buffer_size);
            uint32_t res_id = apir_get_buffer_res_id(buffer);
            printf("[HOST_AFTER] res_id=%u: checksum=0x%08x size=%zu\n",
                   res_id, checksum, buffer_size);
        }
    }
#endif

    // CACHE COHERENCY: Clean up buffers after checksums are complete
    close_specific_session_files_for_guest(ctx->ctx_id, buffer_res_ids.data(), buffer_res_ids.size());

    // CACHE COHERENCY: Flush buffers after computation so guest sees host results
    flush_specific_session_buffers(ctx->ctx_id, buffer_res_ids.data(), buffer_res_ids.size());

    apir_encode_ggml_status(enc, &status);

    return 0;
}
