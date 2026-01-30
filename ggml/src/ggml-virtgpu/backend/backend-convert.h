#include "shared/apir_backend.h"
#include <stdio.h>

#define BUFFER_TO_HOST_HANDLE(name) ggml_buffer_to_apir_handle(name)

static inline apir_buffer_host_handle_t ggml_buffer_to_apir_handle(ggml_backend_buffer_t buffer) {
    // in the backend, the buffer handle is the buffer pointer
    return (apir_buffer_host_handle_t) buffer;
}

#ifndef GGML_BUFFER_TYPE_TO_APIR_HANDLE_DEFINED
#define GGML_BUFFER_TYPE_TO_APIR_HANDLE_DEFINED
static inline apir_buffer_type_host_handle_t ggml_buffer_type_to_apir_handle(ggml_backend_buffer_type_t buft) {
    // in the backend, the buffer handle is the buffer pointer
    apir_buffer_type_host_handle_t handle = (apir_buffer_type_host_handle_t) buft;

    // Validate the conversion
    if (handle == 0) {
        printf("[BACKEND] ERROR: Buffer type conversion resulted in NULL handle!\n");
    } else if (handle < 0x10000) {
        printf("[BACKEND] ERROR: Invalid small handle - backend logic error\n");
    }

    return handle;
}
#endif
