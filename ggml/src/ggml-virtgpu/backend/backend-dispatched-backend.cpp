#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"
#include "shared/apir_backend.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

// CACHE COHERENCY: External functions for buffer management (defined in main.cpp)
extern "C" void unmap_all_session_buffers(uint32_t session_id);
extern "C" void ensure_all_session_buffers_mapped(uint32_t session_id);
extern "C" void close_and_reopen_specific_session_files(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);
extern "C" void close_specific_session_files_for_guest(uint32_t session_id, const uint64_t* buffer_handles, uint32_t num_buffers);

uint32_t backend_backend_graph_compute(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(enc);

    static bool async_backend_initialized = false;
    static bool async_backend;

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

    // Extract unique buffer handles from decoded graph tensors for cache coherency
    std::unordered_set<uint64_t> buffer_handles_set;

    // Process all nodes in the graph
    for (int i = 0; i < cgraph->n_nodes; i++) {
        ggml_tensor * node = cgraph->nodes[i];
        if (node->buffer) {
            // Convert ggml_backend_buffer_t to buffer handle/ID
            uint64_t buffer_handle = (uint64_t)(uintptr_t)node->buffer;
            buffer_handles_set.insert(buffer_handle);
        }

        // Also check source tensors
        for (int j = 0; j < GGML_MAX_SRC && node->src[j]; j++) {
            if (node->src[j]->buffer) {
                uint64_t buffer_handle = (uint64_t)(uintptr_t)node->src[j]->buffer;
                buffer_handles_set.insert(buffer_handle);
            }
        }

        // Check view source
        if (node->view_src && node->view_src->buffer) {
            uint64_t buffer_handle = (uint64_t)(uintptr_t)node->view_src->buffer;
            buffer_handles_set.insert(buffer_handle);
        }
    }

    // Convert to array for passing to C functions
    std::vector<uint64_t> buffer_handles(buffer_handles_set.begin(), buffer_handles_set.end());

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

    // CACHE COHERENCY: Guest/Host file handoff - close host FDs and reopen fresh ones
    close_and_reopen_specific_session_files(ctx->ctx_id, buffer_handles.data(), buffer_handles.size());

    // Run the actual computation
    status = bck->iface.graph_compute(bck, cgraph);

    if (async_backend) {
        bck->iface.synchronize(bck);
    }

    // CACHE COHERENCY: Close host FDs to flush results back to guest
    close_specific_session_files_for_guest(ctx->ctx_id, buffer_handles.data(), buffer_handles.size());

    apir_encode_ggml_status(enc, &status);

    return 0;
}


