#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"
#include "shared/apir_backend.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

uint32_t backend_backend_initialize(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

    // Decode backend initialization request
    uintptr_t function_ptr;
    apir_decode_uintptr_t(dec, &function_ptr);
    void * ggml_backend_reg_fct_p = (void *) function_ptr;

    // Call the actual initialization
    uintptr_t device_handle = 0;
    uint32_t  backend_id    = 0;
    uint32_t  result        = backend_dispatch_initialize(ggml_backend_reg_fct_p, &device_handle, &backend_id);

    // Check if initialization failed
    if (result != APIR_BACKEND_INITIALIZE_SUCCESS) {
        // Return error without encoding anything
        return 1;
    }

    // Encode the device handle and backend ID separately
    apir_encode_uintptr_t(enc, &device_handle);
    apir_encode_uint32_t(enc, &backend_id);

    return 0;
}

static uint32_t validate_graph_operation(size_t cgraph_size, uint32_t shmem_res_id, const char * operation) {
    if (cgraph_size == 0) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Zero-size computation graph\n", operation);
        return 1;
    }

    // place-holder: validate that the size of shmem_res_id is <= cgraph_size
    // need to add another method in the Virgl->APIR callback interface
    GGML_UNUSED(shmem_res_id);

    return 0;  // Valid
}

uint32_t backend_backend_graph_compute(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

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

    // Decode device handle first
    uintptr_t device_handle;
    apir_decode_uintptr_t(dec, &device_handle);
    ggml_backend_dev_t device = (ggml_backend_dev_t) device_handle;

    // Decode backend ID second
    uint32_t backend_id;
    apir_decode_uint32_t(dec, &backend_id);

    // Get backend instance
    apir_backend_instance * instance = get_backend_instance(device, backend_id);
    if (instance == nullptr || instance->bck == nullptr) {
        apir_decoder_set_fatal(dec);
        return 1;
    }

    // Get device context for async property
    apir_device_context * ext = get_device_context(device);
    if (ext == nullptr) {
        apir_decoder_set_fatal(dec);
        return 1;
    }

    if (validate_graph_operation(cgraph_size, shmem_res_id, __func__) != 0) {
        apir_decoder_set_fatal(dec);
        return 1;
    }

    apir_decoder secondary_dec = apir_new_decoder((const char *) shmem_data, cgraph_size);

    ggml_cgraph * cgraph = apir_decode_ggml_cgraph(&secondary_dec, cgraph_size);

    if (!cgraph || apir_decoder_get_fatal(&secondary_dec)) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Failed to deserialize computation graph\n", __func__);
        return 1;
    }

    if (cgraph->n_nodes < 0 || cgraph->n_leafs < 0) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Invalid negative node/leaf count: nodes=%d leafs=%d\n", __func__,
                       cgraph->n_nodes, cgraph->n_leafs);
        return 1;
    }

    ggml_status status;
#if APIR_BACKEND_CHECK_SUPPORTS_OP == 1
    for (int idx = 0; idx < cgraph->n_nodes; idx++) {
        ggml_tensor * op = ggml_graph_node(cgraph, idx);
        if (dev->iface.supports_op(dev, op)) {
            continue;
        }
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Graph node %d (%s) not supported by the backend\n", __func__, idx,
                       ggml_op_desc(op));

        status = GGML_STATUS_ABORTED;
        apir_encode_ggml_status(enc, &status);

        return 0;
    }
#endif

    // Backend instance is already validated above

    status = instance->bck->iface.graph_compute(instance->bck, cgraph);

    if (ext->async_backend && instance->bck->iface.synchronize) {
        instance->bck->iface.synchronize(instance->bck);
    }

    apir_encode_ggml_status(enc, &status);

    return 0;
}

uint32_t backend_backend_cleanup(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(enc);

    // Decode device handle first
    uintptr_t device_handle;
    apir_decode_uintptr_t(dec, &device_handle);
    ggml_backend_dev_t device = (ggml_backend_dev_t) device_handle;

    // Decode backend ID second
    uint32_t backend_id;
    apir_decode_uint32_t(dec, &backend_id);

    // Cleanup specific backend instance
    cleanup_backend_instance(device, backend_id);

    return 0;
}
