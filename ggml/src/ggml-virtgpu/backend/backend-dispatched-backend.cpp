#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"
#include "shared/apir_backend.h"

#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>

// Enable/disable stable pointer caching for CUDA graphs
// WARNING: Stable caching may break CUDA graph tensor relationships
#define ENABLE_STABLE_POINTER_CACHING 0

// Stable pointer caching is now per-device in apir_device_extension

uint32_t backend_backend_initialize(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

    // Decode backend initialization request
    uintptr_t function_ptr;
    apir_decode_uintptr_t(dec, &function_ptr);
    void * ggml_backend_reg_fct_p = (void*)function_ptr;

    // Call the actual initialization
    uintptr_t backend_handle = 0;
    uint32_t result = backend_dispatch_initialize(ggml_backend_reg_fct_p, &backend_handle);

    // Encode the result and handle
    apir_encode_uint32_t(enc, &result);
    apir_encode_uintptr_t(enc, &backend_handle);

    return 0;
}

// SECURITY: Essential validation for computation graph parameters
static uint32_t validate_graph_operation(size_t cgraph_size, const char* operation) {
    // Only check for obviously invalid sizes - no arbitrary limits
    if (cgraph_size == 0) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Zero-size computation graph\n", operation);
        return 1;
    }

    if (cgraph_size < sizeof(ggml_cgraph)) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Graph too small: %zu bytes (min: %zu)\n",
                      operation, cgraph_size, sizeof(ggml_cgraph));
        return 1;
    }

    return 0;  // Valid
}

uint32_t backend_backend_graph_compute(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(enc);

    uint32_t shmem_res_id;
    apir_decode_virtgpu_shmem_res_id(dec, &shmem_res_id);

    const void * shmem_data = ctx->iface->get_shmem_ptr(ctx->ctx_id, shmem_res_id);
    if (!shmem_data) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Couldn't get the shmem addr from virgl\n", __func__);
        apir_decoder_set_fatal(dec);
        return 1;
    }
    size_t cgraph_size;
    apir_decode_size_t(dec, &cgraph_size);

    // Decode combined handle (device + backend ID)
    uintptr_t combined_handle;
    apir_decode_uintptr_t(dec, &combined_handle);

    // Decode device and backend ID from handle
    ggml_backend_dev_t device = (ggml_backend_dev_t)(combined_handle >> 32);
    uintptr_t backend_id = combined_handle & 0xFFFFFFFF;

    // Get backend instance
    apir_backend_instance* instance = get_backend_instance(device, backend_id);
    if (instance == nullptr || instance->bck == nullptr) {
        apir_decoder_set_fatal(dec);
        return 1;
    }

    // Get async backend properties from the device
    static bool async_backend_initialized = false;
    static bool async_backend;

    if (!async_backend_initialized) {
        ggml_backend_dev_props props;
        device->iface.get_props(device, &props);
        async_backend = props.caps.async;
        async_backend_initialized = true;
    }

    // Decode frontend cgraph key for caching
    uintptr_t frontend_key;
    apir_decode_uintptr_t(dec, &frontend_key);

    // SECURITY: Validate graph size before processing
    if (validate_graph_operation(cgraph_size, __func__) != 0) {
        apir_decoder_set_fatal(dec);
        return 1;
    }

    apir_decoder secondary_dec = apir_new_decoder((const char *) shmem_data, cgraph_size);

    ggml_cgraph * cgraph = apir_decode_ggml_cgraph(&secondary_dec, cgraph_size);

    // SECURITY: Validate graph deserialization succeeded
    if (!cgraph || apir_decoder_get_fatal(&secondary_dec)) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Failed to deserialize computation graph\n", __func__);
        return 1;
    }

    // SECURITY: Basic graph validation - no arbitrary limits
    if (cgraph->n_nodes < 0 || cgraph->n_leafs < 0) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Invalid negative node/leaf count: nodes=%d leafs=%d\n",
                      __func__, cgraph->n_nodes, cgraph->n_leafs);
        return 1;
    }

#if ENABLE_STABLE_POINTER_CACHING
    // Stable pointer caching for CUDA graph keys
    // WARNING: This may break CUDA graph tensor relationships
    void** cached_stable_nodes = nullptr;

    {
        std::lock_guard<std::mutex> lock(instance->cache_mutex);
        auto it = instance->stable_nodes_cache.find(frontend_key);
        if (it != instance->stable_nodes_cache.end()) {
            cached_stable_nodes = it->second;
        }
    }

    // Reuse cached stable node array if structure matches
    if (cached_stable_nodes && instance->stable_nodes_count[frontend_key] == cgraph->n_nodes) {

        // CRITICAL: Save fresh nodes before replacing with cached stable ones
        struct ggml_tensor** fresh_nodes = cgraph->nodes;

        // Replace cgraph->nodes with stable cached array (stable addresses for CUDA)
        cgraph->nodes = (struct ggml_tensor**)cached_stable_nodes;

        // CRITICAL: Copy ALL tensor data to cached stable tensors while preserving addresses
        // This updates data, buffer, extra, view_src, view_offs, src pointers, and all other fields
        for (int i = 0; i < cgraph->n_nodes; i++) {
            memcpy(cgraph->nodes[i], fresh_nodes[i], sizeof(struct ggml_tensor));
        }

    } else {
        // Create new stable nodes array
        std::lock_guard<std::mutex> lock(instance->cache_mutex);

        // Allocate persistent nodes array
        void** stable_nodes = (void**)malloc(sizeof(void*) * cgraph->n_nodes);
        if (!stable_nodes) {
            return 1;
        }
        // Copy current node pointers to stable array
        for (int i = 0; i < cgraph->n_nodes; i++) {
            stable_nodes[i] = (void*)cgraph->nodes[i];
        }

        // Cache the stable array in backend instance
        instance->stable_nodes_cache[frontend_key] = stable_nodes;
        instance->stable_nodes_count[frontend_key] = cgraph->n_nodes;

        // Use stable array
        cgraph->nodes = (struct ggml_tensor**)stable_nodes;
    }
#endif


    ggml_status status;
#if APIR_BACKEND_CHECK_SUPPORTS_OP == 1
    for (int idx = 0; idx < cgraph->n_nodes; idx++) {
        ggml_tensor * op = ggml_graph_node(cgraph, idx);
        if (dev->iface.supports_op(dev, op)) {
            continue;
        }
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Graph node %d (%s) not supported by the backend\n",
                       __func__, idx, ggml_op_desc(op));

        status = GGML_STATUS_ABORTED;
        apir_encode_ggml_status(enc, &status);

        return 0;
    }
#endif

    // Backend instance is already validated above

    status = instance->bck->iface.graph_compute(instance->bck, cgraph);

    if (async_backend && instance->bck->iface.synchronize) {
        instance->bck->iface.synchronize(instance->bck);
    }

    apir_encode_ggml_status(enc, &status);

    return 0;
}

uint32_t backend_backend_cleanup(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(enc);

    // Decode combined handle (device + backend ID)
    uintptr_t combined_handle;
    apir_decode_uintptr_t(dec, &combined_handle);

    // Decode device and backend ID from handle
    ggml_backend_dev_t device = (ggml_backend_dev_t)(combined_handle >> 32);
    uintptr_t backend_id = combined_handle & 0xFFFFFFFF;

    // Cleanup specific backend instance
    cleanup_backend_instance(device, backend_id);

    return 0;
}
