#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include <cstdint>

uint32_t backend_device_get_device_count(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    int32_t dev_count = reg->iface.get_device_count(reg);
    apir_encode_int32_t(enc, &dev_count);

    return 0;
}

uint32_t backend_device_get_count(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    printf("[BACKEND] ====> backend_device_get_count: Called\n");

    if (reg == NULL) {
        printf("[BACKEND] ERROR: backend_device_get_count: reg is NULL\n");
        return 1;
    }

    int32_t dev_count = reg->iface.get_device_count(reg);
    printf("[BACKEND] ====> backend_device_get_count: Registry reports %d devices\n", dev_count);

    // INSTRUMENTATION: Exit if more than 2 devices found
    if (dev_count > 2) {
        printf("[BACKEND] FATAL: Found %d devices - this is abnormal! Expected 1-2 devices maximum.\n", dev_count);
        printf("[BACKEND] FATAL: This suggests a serious backend registry error.\n");
        printf("[BACKEND] FATAL: Terminating to prevent corruption.\n");
        exit(1);
    }

    apir_encode_int32_t(enc, &dev_count);
    printf("[BACKEND] ====> backend_device_get_count: Encoded %d as response\n", dev_count);

    return 0;
}

uint32_t backend_device_get_name(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    const char * string = dev->iface.get_name(dev);

    const size_t string_size = strlen(string) + 1;
    apir_encode_array_size(enc, string_size);
    apir_encode_char_array(enc, string, string_size);

    return 0;
}

uint32_t backend_device_get_description(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    const char * string = dev->iface.get_description(dev);

    const size_t string_size = strlen(string) + 1;
    apir_encode_array_size(enc, string_size);
    apir_encode_char_array(enc, string, string_size);

    return 0;
}

uint32_t backend_device_get_type(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    uint32_t type = dev->iface.get_type(dev);
    apir_encode_uint32_t(enc, &type);

    return 0;
}

uint32_t backend_device_get_memory(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    size_t free, total;
    dev->iface.get_memory(dev, &free, &total);

    apir_encode_size_t(enc, &free);
    apir_encode_size_t(enc, &total);

    return 0;
}

uint32_t backend_device_supports_op(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

    const ggml_tensor * op = apir_decode_ggml_tensor_inplace(dec);

    bool supports_op = dev->iface.supports_op(dev, op);

    apir_encode_bool_t(enc, &supports_op);

    return 0;
}

uint32_t backend_device_get_buffer_type(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    if (reg == NULL) {
        return 1;
    }

    if (dev == NULL) {
        return 1;
    }

    ggml_backend_buffer_type_t bufft = dev->iface.get_buffer_type(dev);

    // Backend validation: Check buffer type handle before encoding
    printf("[BACKEND] ====> backend_device_get_buffer_type: Raw buffer type handle = %p\n",
           (void*)bufft);

    if (bufft == NULL) {
        printf("[BACKEND] ERROR: Device returned NULL buffer type!\n");
        return 1;
    }

    if ((uintptr_t)bufft < 0x10000) {
        printf("[BACKEND] ERROR: Device returned invalid buffer type handle %p - too small for pointer\n", (void*)bufft);
        printf("[BACKEND] ERROR: This indicates a backend logic error, not data corruption\n");
        return 1;
    }

    printf("[BACKEND] Buffer type handle validation passed - encoding %p\n", (void*)bufft);
    apir_encode_ggml_buffer_type(enc, bufft);

    return 0;
}

uint32_t backend_device_get_props(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    ggml_backend_dev_props props;
    dev->iface.get_props(dev, &props);

    printf("[BACKEND_DISPATCHER] CPU backend returned:\n");
    printf("[BACKEND_DISPATCHER]   async = %s\n", props.caps.async ? "true" : "false");
    printf("[BACKEND_DISPATCHER]   host_buffer = %s\n", props.caps.host_buffer ? "true" : "false");
    printf("[BACKEND_DISPATCHER]   buffer_from_host_ptr = %s\n", props.caps.buffer_from_host_ptr ? "true" : "false");
    printf("[BACKEND_DISPATCHER]   events = %s\n", props.caps.events ? "true" : "false");

    printf("[BACKEND_DISPATCHER] About to encode distinctive hex values:\n");

    // Send distinctive hex values instead of actual booleans
    uint32_t test_async = 0xAAAA1111;
    uint32_t test_host_buffer = 0xBBBB2222;
    uint32_t test_buffer_from_host_ptr = 0xCCCC3333;
    uint32_t test_events = 0xDDDD4444;

    printf("[BACKEND_DISPATCHER]   Encoding async = 0x%08X\n", test_async);
    apir_encode_uint32_t(enc, &test_async);

    printf("[BACKEND_DISPATCHER]   Encoding host_buffer = 0x%08X\n", test_host_buffer);
    apir_encode_uint32_t(enc, &test_host_buffer);

    printf("[BACKEND_DISPATCHER]   Encoding buffer_from_host_ptr = 0x%08X\n", test_buffer_from_host_ptr);
    apir_encode_uint32_t(enc, &test_buffer_from_host_ptr);

    printf("[BACKEND_DISPATCHER]   Encoding events = 0x%08X\n", test_events);
    apir_encode_uint32_t(enc, &test_events);

    printf("[BACKEND_DISPATCHER] All values encoded successfully\n");
    fflush(stdout);

    return 0;
}

uint32_t backend_device_buffer_from_ptr(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);

    uint32_t shmem_res_id;
    apir_decode_virtgpu_shmem_res_id(dec, &shmem_res_id);

    void * shmem_ptr = ctx->iface->get_shmem_ptr(ctx->ctx_id, shmem_res_id);
    if (!shmem_ptr) {
        GGML_LOG_ERROR("Couldn't get the shmem addr from virgl\n");
        apir_decoder_set_fatal(dec);
        return 1;
    }

    size_t size;
    apir_decode_size_t(dec, &size);
    size_t max_tensor_size;
    apir_decode_size_t(dec, &max_tensor_size);

    ggml_backend_buffer_t buffer;
    buffer = dev->iface.buffer_from_host_ptr(dev, shmem_ptr, size, max_tensor_size);

    apir_encode_ggml_buffer(enc, buffer);
    apir_encode_ggml_buffer_type(enc, buffer->buft);

    if (buffer) {
        apir_track_backend_buffer(buffer);
    }

    return 0;
}
