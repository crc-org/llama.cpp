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

    // Fool check: Validate the conversion
    printf("[BACKEND] ggml_buffer_type_to_apir_handle: input=%p -> output=%p\n",
           (void*)buft, (void*)(uintptr_t)handle);

    if (handle == 0) {
        printf("[BACKEND] ERROR: Buffer type conversion resulted in NULL handle!\n");
    } else if (handle < 0x10000) {
        printf("[BACKEND] ERROR: Buffer type conversion resulted in invalid small handle %p!\n",
               (void*)(uintptr_t)handle);
        printf("[BACKEND] ERROR: Input buffer type was %p - this suggests backend logic error\n", (void*)buft);
    } else {
        printf("[BACKEND] Buffer type conversion OK: %p -> %p\n",
               (void*)buft, (void*)(uintptr_t)handle);
    }

    return handle;
}
#endif
