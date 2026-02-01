#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"
#include "ggml.h"

#include <cstdint>

// Simple checksum for data verification
static uint32_t simple_checksum(const void * data, size_t size) {
    const uint8_t * bytes = (const uint8_t *)data;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum * 31) + bytes[i];
    }
    return checksum;
}

// Static IDs for matching guest/host operations
static uint32_t buffer_set_tensor_id = 0;
static uint32_t buffer_get_tensor_id = 0;

// CACHE COHERENCY: External function to ensure all session buffers are mapped
extern "C" void ensure_all_session_buffers_mapped(uint32_t session_id);

uint32_t backend_buffer_get_base(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_t buffer;
    buffer = apir_decode_ggml_buffer(dec);

    if (!buffer) {
        printf("[ERROR] backend_buffer_get_base: buffer is NULL\n");
        return 1;
    }

    if (!buffer->iface.get_base) {
        printf("[ERROR] backend_buffer_get_base: buffer->iface.get_base is NULL\n");
        return 1;
    }

    uintptr_t base = (uintptr_t) buffer->iface.get_base(buffer);
    apir_encode_uintptr_t(enc, &base);

    return 0;
}

uint32_t backend_buffer_set_tensor(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(enc);

    // CACHE COHERENCY: Ensure all session buffers are mapped before buffer operations
    ensure_all_session_buffers_mapped(ctx->ctx_id);

    // Decode verification message with guest checksum
    apir_buffer_host_handle_t buffer_handle;
    apir_decode_apir_buffer_host_handle_t(dec, &buffer_handle);

    uint32_t buffer_res_id;
    apir_decode_virtgpu_shmem_res_id(dec, &buffer_res_id);

    size_t offset;
    apir_decode_size_t(dec, &offset);

    size_t size;
    apir_decode_size_t(dec, &size);

    uint32_t guest_checksum;
    apir_decode_uint32_t(dec, &guest_checksum);

    // Assign matching static ID
    uint32_t operation_id = ++buffer_set_tensor_id;

    // Function static cache coherency counters for set_tensor
    static uint32_t set_success_count = 0;
    static uint32_t set_failure_count = 0;

    // Convert handle back to buffer pointer and verify it's tracked
    ggml_backend_buffer_t buffer = (ggml_backend_buffer_t)(uintptr_t)buffer_handle;

    // Verify the buffer is in our tracked set
    auto tracked_buffers = apir_get_track_backend_buffers();
    if (tracked_buffers.find(buffer) == tracked_buffers.end()) {
        printf("HOST  #%u res_id=%u ERROR: Buffer handle=%lu not tracked or invalid\n",
               operation_id, buffer_res_id, (unsigned long)buffer_handle);
        set_failure_count++;
        return 1;
    }

    // Get buffer base to calculate checksum
    void * buffer_base = ggml_backend_buffer_get_base(buffer);
    if (!buffer_base) {
        printf("HOST  #%u res_id=%u ERROR: Cannot get buffer base for handle=%lu\n",
               operation_id, buffer_res_id, (unsigned long)buffer_handle);
        set_failure_count++;
        return 1;
    }

    // Calculate checksum of the buffer region
    uint32_t host_checksum = simple_checksum((char *)buffer_base + offset, size);

    // Compare checksums and update counters
    if (guest_checksum == host_checksum) {
        printf("HOST  #%u res_id=%u SET_CACHE_SUCCESS: guest=0x%08x host=0x%08x (SET_SUCCESS: %u, SET_FAILURES: %u)\n",
               operation_id, buffer_res_id, guest_checksum, host_checksum,
               ++set_success_count, set_failure_count);
    } else {
        printf("HOST  #%u res_id=%u SET_CACHE_FAILURE: guest=0x%08x host=0x%08x (SET_SUCCESS: %u, SET_FAILURES: %u)\n",
               operation_id, buffer_res_id, guest_checksum, host_checksum,
               set_success_count, ++set_failure_count);

        // Continue execution instead of aborting - let's see how many fail
        if (host_checksum == 0x00000000) {
            printf("HOST  #%u res_id=%u NOTE: Host buffer contains all zeros - cache coherency broken!\n",
                   operation_id, buffer_res_id);
        }
    }
#define ABORT_ON_INCONSISTENCY 1
#if ABORT_ON_INCONSISTENCY == 1
    if (set_failure_count) {
        printf("[HOST] set_tensor cache coherency broken, aborting!\n");

        set_failure_count = 0;
        set_success_count = 0;
        return 1;
    }
#endif
    return 0;
}

uint32_t backend_buffer_get_tensor(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(enc);

    // CACHE COHERENCY: Ensure all session buffers are mapped before buffer operations
    ensure_all_session_buffers_mapped(ctx->ctx_id);

    // Decode minimal verification message (matches guest's apir_buffer_get_tensor)
    apir_buffer_host_handle_t buffer_handle;
    apir_decode_apir_buffer_host_handle_t(dec, &buffer_handle);

    uint32_t buffer_res_id;
    apir_decode_virtgpu_shmem_res_id(dec, &buffer_res_id);

    size_t offset;
    apir_decode_size_t(dec, &offset);

    size_t size;
    apir_decode_size_t(dec, &size);

    uint32_t guest_checksum;
    apir_decode_uint32_t(dec, &guest_checksum);

    // Assign matching static ID
    uint32_t operation_id = ++buffer_get_tensor_id;

    // Function static cache coherency counters for get_tensor
    static uint32_t get_success_count = 0;
    static uint32_t get_failure_count = 0;

    // Convert handle back to buffer pointer and verify it's tracked
    ggml_backend_buffer_t buffer = (ggml_backend_buffer_t)(uintptr_t)buffer_handle;

    // Verify the buffer is in our tracked set
    auto tracked_buffers = apir_get_track_backend_buffers();
    if (tracked_buffers.find(buffer) == tracked_buffers.end()) {
        printf("HOST  #%u res_id=%u ERROR: Buffer handle=%lu not tracked or invalid\n",
               operation_id, buffer_res_id, (unsigned long)buffer_handle);
        get_failure_count++;
        return 1;
    }

    // Get buffer base to calculate checksum
    void * buffer_base = ggml_backend_buffer_get_base(buffer);
    if (!buffer_base) {
        printf("HOST  #%u res_id=%u ERROR: Cannot get buffer base for handle=%lu\n",
               operation_id, buffer_res_id, (unsigned long)buffer_handle);
        get_failure_count++;
        return 1;
    }

    // Calculate checksum of the buffer region
    uint32_t host_checksum = simple_checksum((char *)buffer_base + offset, size);

    // Compare checksums and update counters
    if (guest_checksum == host_checksum) {
        printf("HOST  #%u res_id=%u GET_CACHE_SUCCESS: guest=0x%08x host=0x%08x (GET_SUCCESS: %u, GET_FAILURES: %u)\n",
               operation_id, buffer_res_id, guest_checksum, host_checksum,
               ++get_success_count, get_failure_count);
    } else {
        printf("HOST  #%u res_id=%u GET_CACHE_FAILURE: guest=0x%08x host=0x%08x (GET_SUCCESS: %u, GET_FAILURES: %u)\n",
               operation_id, buffer_res_id, guest_checksum, host_checksum,
               get_success_count, ++get_failure_count);

        // Continue execution instead of aborting - let's see how many fail
        if (host_checksum == 0x00000000) {
            printf("HOST  #%u res_id=%u NOTE: Host buffer contains all zeros - cache coherency broken!\n",
                   operation_id, buffer_res_id);
        }
    }

#define ABORT_ON_INCONSISTENCY 1
#if ABORT_ON_INCONSISTENCY == 1
    if (get_failure_count) {
        printf("[HOST] get_tensor cache coherency broken, aborting!\n");

        get_failure_count = 0;
        get_success_count = 0;
        return 1;
    }
#endif
    return 0;
}

uint32_t backend_buffer_cpy_tensor(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

    ggml_backend_buffer_t buffer;
    buffer = apir_decode_ggml_buffer(dec);

    const ggml_tensor * src;
    // safe to remove the const qualifier here
    src               = apir_decode_ggml_tensor(dec);
    ggml_tensor * dst = (ggml_tensor *) (uintptr_t) apir_decode_ggml_tensor(dec);

    bool ret = buffer->iface.cpy_tensor(buffer, src, (ggml_tensor *) dst);

    apir_encode_bool_t(enc, &ret);

    return 0;
}

uint32_t backend_buffer_clear(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(enc);

    // CACHE COHERENCY: Ensure all session buffers are mapped before buffer operations
    ensure_all_session_buffers_mapped(ctx->ctx_id);

    ggml_backend_buffer_t buffer;
    buffer = apir_decode_ggml_buffer(dec);

    if (!buffer) {
        printf("[ERROR] backend_buffer_clear: buffer is NULL\n");
        return 1;
    }

    if (!buffer->iface.clear) {
        printf("[ERROR] backend_buffer_clear: buffer->iface.clear is NULL\n");
        return 1;
    }

    uint8_t value;
    apir_decode_uint8_t(dec, &value);

    buffer->iface.clear(buffer, value);

    return 0;
}

uint32_t backend_buffer_free_buffer(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(enc);

    ggml_backend_buffer_t buffer;
    buffer = apir_decode_ggml_buffer(dec);

    if (!buffer) {
        printf("[ERROR] backend_buffer_free_buffer: buffer is NULL - aborting\n");
        return 1;
    }


    // if buffer is not owned, no need to free it
    if (buffer->iface.free_buffer) {
        buffer->iface.free_buffer(buffer);
    }

    if (!apir_untrack_backend_buffer(buffer)) {
        GGML_LOG_WARN("%s: unknown buffer %p\n", __func__, (void *) buffer);
        return 1;
    }


    return 0;
}
