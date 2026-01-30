#include "backend-dispatched.h"
#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>

uint32_t backend_buffer_type_get_name(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);

    ggml_backend_buffer_type_t buft = apir_decode_ggml_buffer_type(dec);

    // Fool check: Comprehensive buffer type handle validation
    printf("[BACKEND] backend_buffer_type_get_name: Received buffer type handle = %p\n",
           (void*)buft);

    if (buft == NULL || (uintptr_t)buft < 0x10000) {
        printf("[ERROR] backend_buffer_type_get_name: Invalid buffer type handle %p\n", (void*)buft);
        return 1;
    }

    printf("[BACKEND] Buffer type handle validation passed for get_name\n");

    const char * string = buft->iface.get_name(buft);
    printf("[BACKEND] get_name returned: '%s'\n", string ? string : "NULL");

    const size_t string_size = strlen(string) + 1;
    apir_encode_array_size(enc, string_size);
    apir_encode_char_array(enc, string, string_size);

    return 0;
}

uint32_t backend_buffer_type_get_alignment(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_type_t buft;
    buft = apir_decode_ggml_buffer_type(dec);

    // Fool check: Comprehensive buffer type handle validation
    printf("[BACKEND] backend_buffer_type_get_alignment: Received buffer type handle = %p\n",
           (void*)buft);

    if (!buft) {
        printf("[ERROR] backend_buffer_type_get_alignment: buft is NULL\n");
        return 1; // Return error code
    }

    if ((uintptr_t)buft < 0x10000) {
        printf("[ERROR] backend_buffer_type_get_alignment: Invalid buffer type handle %p - too small for pointer\n",
               (void*)buft);
        printf("[ERROR] This indicates corrupted data or backend logic error\n");
        return 1;
    }

    printf("[BACKEND] Buffer type handle validation passed for get_alignment\n");

    size_t value = 0;
    if (buft->iface.get_alignment) {
        value = buft->iface.get_alignment(buft);
        printf("[BACKEND] get_alignment returned: %zu\n", value);
    } else {
        printf("[BACKEND] No get_alignment interface, using default 0\n");
    }

    apir_encode_size_t(enc, &value);
    return 0;
}

uint32_t backend_buffer_type_get_max_size(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx) {
    GGML_UNUSED(ctx);
    ggml_backend_buffer_type_t buft;
    buft = apir_decode_ggml_buffer_type(dec);

    // Fool check: Comprehensive buffer type handle validation
    printf("[BACKEND] backend_buffer_type_get_max_size: Received buffer type handle = %p\n",
           (void*)buft);

    if (!buft) {
        printf("[ERROR] backend_buffer_type_get_max_size: buft is NULL\n");
        return 1; // Return error code
    }

    if ((uintptr_t)buft < 0x10000) {
        printf("[ERROR] backend_buffer_type_get_max_size: Invalid buffer type handle %p - too small for pointer\n",
               (void*)buft);
        printf("[ERROR] This indicates corrupted data or backend logic error\n");
        return 1;
    }

    printf("[BACKEND] Buffer type handle validation passed for get_max_size\n");

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

    // Fool check: Comprehensive buffer type handle validation
    printf("[BACKEND] backend_buffer_type_alloc_buffer: Received buffer type handle = %p\n",
           (void*)buft);

    if (!buft) {
        printf("[ERROR] backend_buffer_type_alloc_buffer: buft is NULL\n");
        return 1;
    }

    if ((uintptr_t)buft < 0x10000) {
        printf("[ERROR] backend_buffer_type_alloc_buffer: Invalid buffer type handle %p - too small for pointer\n",
               (void*)buft);
        printf("[ERROR] This indicates corrupted data or backend logic error\n");
        return 1;
    }

    printf("[BACKEND] Buffer type handle validation passed for alloc_buffer\n");

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

    // Fool check: Comprehensive buffer type handle validation
    printf("[BACKEND] backend_buffer_type_get_alloc_size: Received buffer type handle = %p\n",
           (void*)buft);

    if (!buft) {
        printf("[ERROR] backend_buffer_type_get_alloc_size: buft is NULL\n");
        return 1;
    }

    if ((uintptr_t)buft < 0x10000) {
        printf("[ERROR] backend_buffer_type_get_alloc_size: Invalid buffer type handle %p - too small for pointer\n",
               (void*)buft);
        printf("[ERROR] This indicates corrupted data or backend logic error\n");
        return 1;
    }

    printf("[BACKEND] Buffer type handle validation passed for get_alloc_size\n");

    const ggml_tensor * op = apir_decode_ggml_tensor_inplace(dec);

    size_t value = buft->iface.get_alloc_size(buft, op);
    printf("[BACKEND] get_alloc_size returned: %zu\n", value);

    apir_encode_size_t(enc, &value);

    return 0;
}
