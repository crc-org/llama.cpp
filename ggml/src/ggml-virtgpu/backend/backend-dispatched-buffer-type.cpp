#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>

#define CHECK_BUFT(buft) \
    do {                                                                \
        printf("[BACKEND] %s: Received buffer type handle = %p\n", __func__, \
               (void*)buft);                                            \
                                                                        \
        if (buft == NULL || (uintptr_t)buft < 0x10000 || (uintptr_t)buft == 0x0000000A00000002) { \
            printf("Unexpected buft. Aborting.\n");                     \
            return 1;                                                   \
        }                                                               \
    } while(0)

uint32_t backend_buffer_type_get_name(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

    ggml_backend_buffer_type_t buft = apir_decode_ggml_buffer_type(dec);

    CHECK_BUFT(buft);

    const char * string = buft->iface.get_name(buft);

    const size_t string_size = strlen(string) + 1;
    apir_encode_array_size(enc, string_size);
    apir_encode_char_array(enc, string, string_size);

    return 0;
}

uint32_t backend_buffer_type_get_alignment(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_type_t buft;
    buft = apir_decode_ggml_buffer_type(dec);

    CHECK_BUFT(buft);

    size_t value = 0;
    if (buft->iface.get_alignment) {
        value = buft->iface.get_alignment(buft);
    }

    apir_encode_size_t(enc, &value);
    return 0;
}

uint32_t backend_buffer_type_get_max_size(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_type_t buft;
    buft = apir_decode_ggml_buffer_type(dec);

    CHECK_BUFT(buft);

    size_t value = SIZE_MAX;
    if (buft->iface.get_max_size) {
        value = buft->iface.get_max_size(buft);
        printf("[BACKEND] get_max_size returned: %zu\n", value);
    } else {
        printf("[BACKEND] No get_max_size interface, using default SIZE_MAX\n");
    }

    apir_encode_size_t(enc, &value);

    return 0;
}

/* APIR_COMMAND_TYPE_BUFFER_TYPE_IS_HOST is deprecated. Keeping the handler for backward compatibility. */
uint32_t backend_buffer_type_is_host(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    GGML_UNUSED(dec);
    const bool is_host = false;

    apir_encode_bool_t(enc, &is_host);

    return 0;
}

uint32_t backend_buffer_type_alloc_buffer(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_type_t buft;
    buft = apir_decode_ggml_buffer_type(dec);

    CHECK_BUFT(buft);

    size_t size;
    apir_decode_size_t(dec, &size);
    printf("[BACKEND] Allocating buffer of size: %zu\n", size);

    ggml_backend_buffer_t buffer;

    buffer = buft->iface.alloc_buffer(buft, size);
    printf("[BACKEND] alloc_buffer returned: %p\n", (void*)buffer);

    apir_encode_ggml_buffer(enc, buffer);

    if (buffer) {
        apir_track_backend_buffer(buffer);
    }

    return 0;
}

uint32_t backend_buffer_type_get_alloc_size(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_type_t buft;
    buft = apir_decode_ggml_buffer_type(dec);

    CHECK_BUFT(buft);

    const ggml_tensor * op = apir_decode_ggml_tensor_inplace(dec);

    size_t value = buft->iface.get_alloc_size(buft, op);
    printf("[BACKEND] get_alloc_size returned: %zu\n", value);

    apir_encode_size_t(enc, &value);

    return 0;
}
