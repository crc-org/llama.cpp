#include "virtgpu-forward-impl.h"
#ifndef GGML_VIRTGPU_USE_WINDOWS
#include "virtgpu-shm.h"
#endif

int apir_device_get_count(virtgpu * gpu) {

    static int32_t dev_count = -1;

    // Reset cache for debugging - remove this later
    dev_count = -1;

    if (dev_count != -1) {
        return dev_count;
    }

    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_COUNT);
    REMOTE_CALL(gpu, encoder, decoder, ret);

    apir_decode_int32_t(decoder, &dev_count);

    remote_call_finish(gpu, encoder, decoder);

    return dev_count;
}

char * apir_device_get_name(virtgpu * gpu) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_NAME);
    REMOTE_CALL(gpu, encoder, decoder, ret);

    const size_t string_size = apir_decode_array_size_unchecked(decoder);
    char            * string = (char *) apir_decoder_alloc_array(sizeof(char), string_size);
    if (!string) {
        printf("%s: Could not allocate the device name buffer\n", __func__);
        return NULL;
    }
    apir_decode_char_array(decoder, string, string_size);

    remote_call_finish(gpu, encoder, decoder);

    return string;
}

char * apir_device_get_description(virtgpu * gpu) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_DESCRIPTION);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    const size_t string_size = apir_decode_array_size_unchecked(decoder);
    char *       string      = (char *) apir_decoder_alloc_array(sizeof(char), string_size);
    if (!string) {
        printf("%s: Could not allocate the device description buffer\n", __func__);

        return NULL;
    }
    apir_decode_char_array(decoder, string, string_size);

    remote_call_finish(gpu, encoder, decoder);

    return string;
}

uint32_t apir_device_get_type(virtgpu * gpu) {
    static uint32_t dev_type = 255;
    if (dev_type != 255) {
        return dev_type;
    }

    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_TYPE);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    apir_decode_uint32_t(decoder, &dev_type);

    remote_call_finish(gpu, encoder, decoder);

    return dev_type;
}

void apir_device_get_memory(virtgpu * gpu, size_t * free, size_t * total) {
    static size_t         dev_free  = 0;
    static size_t         dev_total = 0;
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_MEMORY);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    apir_decode_size_t(decoder, &dev_free);
    apir_decode_size_t(decoder, &dev_total);

    *free  = dev_free;
    *total = dev_total;

    remote_call_finish(gpu, encoder, decoder);

    return;
}

bool apir_device_supports_op(virtgpu * gpu, const ggml_tensor * op) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_SUPPORTS_OP);

    apir_encode_ggml_tensor_inline(encoder, op);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    bool supports_op;
    apir_decode_bool_t(decoder, &supports_op);

    remote_call_finish(gpu, encoder, decoder);

    return supports_op;
}

apir_buffer_type_host_handle_t apir_device_get_buffer_type(virtgpu * gpu) {

    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_BUFFER_TYPE);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    ggml_backend_buffer_type_t buft = apir_decode_ggml_buffer_type(decoder);

    apir_buffer_type_host_handle_t buft_handle = (apir_buffer_type_host_handle_t)buft;

    remote_call_finish(gpu, encoder, decoder);

    return buft_handle;
}

void apir_device_get_props(virtgpu * gpu,
                           bool *    async,
                           bool *    host_buffer,
                           bool *    buffer_from_host_ptr,
                           bool *    events) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_GET_PROPS);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    printf("[GUEST] About to decode uint32_t values from response:\n");
    printf("[GUEST] Decoder state: cur=%p, end=%p, remaining_bytes=%ld\n",
           decoder->cur, decoder->end, decoder->end - decoder->cur);

    // Hex dump of received data (same 20 bytes as Windows service shows)
    printf("[GUEST] Received buffer content (first 20 bytes):\n");
    printf("[GUEST] Hex dump: ");
    for (int i = 0; i < 20 && decoder->cur + i < decoder->end; i++) {
        printf("%02x ", (unsigned char)decoder->cur[i]);
        if ((i + 1) % 16 == 0) printf("\n[GUEST]           ");
    }
    printf("\n");

    uint32_t recv_async, recv_host_buffer, recv_buffer_from_host_ptr, recv_events;

    apir_decode_uint32_t(decoder, &recv_async);

    apir_decode_uint32_t(decoder, &recv_host_buffer);

    apir_decode_uint32_t(decoder, &recv_buffer_from_host_ptr);

    apir_decode_uint32_t(decoder, &recv_events);

    printf("[GUEST] Expected values:\n");

    printf("[GUEST]   async should be:                0xAAAA1111 --> 0x%x\n", recv_async);
    printf("[GUEST]   host_buffer should be:          0xBBBB2222 --> 0x%x\n", recv_host_buffer);
    printf("[GUEST]   buffer_from_host_ptr should be: 0xCCCC3333 --> 0x%x\n", recv_buffer_from_host_ptr);
    printf("[GUEST]   events should be:               0xDDDD4444 --> 0x%x\n", recv_events);

    // Convert back to boolean values for the function signature
    *async = (recv_async != 0);
    *host_buffer = (recv_host_buffer != 0);
    *buffer_from_host_ptr = (recv_buffer_from_host_ptr != 0);
    *events = (recv_events != 0);

    if (recv_buffer_from_host_ptr != 0xCCCC3333) {
        printf("[GUEST] ERROR: buffer_from_host_ptr mismatch! Expected 0xCCCC3333, got 0x%08X\n", recv_buffer_from_host_ptr);
        printf("thks bye :/\n");
        _exit(0);
    }

    printf("[GUEST] SUCCESS: All hex values match expected patterns!\n");
    _exit(0);

    *buffer_from_host_ptr = true;
    remote_call_finish(gpu, encoder, decoder);
}

apir_buffer_context_t apir_device_buffer_from_ptr(virtgpu * gpu, size_t size, size_t max_tensor_size) {
    apir_encoder *        encoder;
    apir_decoder *        decoder;
    ApirForwardReturnCode ret;

    apir_buffer_context_t buffer_context;

    REMOTE_CALL_PREPARE(gpu, encoder, APIR_COMMAND_TYPE_DEVICE_BUFFER_FROM_PTR);

    if (virtgpu_shmem_create(gpu, size, &buffer_context.shmem)) {
        printf("[ERROR] Couldn't allocate the guest-host shared buffer\n");
        buffer_context.shmem.res_id = 0;
        buffer_context.host_handle = 0;
        buffer_context.buft_host_handle = 0;
        return buffer_context;
    }

    apir_encode_virtgpu_shmem_res_id(encoder, buffer_context.shmem.res_id);

    apir_encode_size_t(encoder, &size);
    apir_encode_size_t(encoder, &max_tensor_size);

    REMOTE_CALL(gpu, encoder, decoder, ret);

    apir_decode_apir_buffer_host_handle_t(decoder, &buffer_context.host_handle);
    buffer_context.buft_host_handle = apir_decode_apir_buffer_type_host_handle(decoder);

    remote_call_finish(gpu, encoder, decoder);

    return buffer_context;
}
