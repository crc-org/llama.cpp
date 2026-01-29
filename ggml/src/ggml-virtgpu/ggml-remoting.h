#pragma once

#include <stdio.h>
#include "../ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include "backend/shared/apir_backend.h"

#ifndef GGML_VIRTGPU_USE_WINDOWS
#include "virtgpu.h"
#else
#include "virtgpu-interface.h"
#include "winApiRmt.h"
#include "ggml-winapi-client.h"
#include "apir-minimal.h"
#endif

#include "virtgpu-forward.gen.h"

#include <memory>
#include <string>
#include <vector>
#include <tuple>

// USE_ALWAYS_TRUE_SUPPORTS_OP: 1 is fast, 0 avoid micro-benchmark crashes

#define USE_ALWAYS_TRUE_SUPPORTS_OP 1
#define USE_METAL_GUEST_SUPPORTS_OP 0

#define DEV_TO_GPU(name) ((ggml_backend_remoting_device_context *) (name)->context)->gpu

#define BUFFER_TO_GGML_CONTEXT(name) ((ggml_backend_remoting_buffer_context *) (name)->context)

#define BUFFER_TO_APIR_CONTEXT(name) &((ggml_backend_remoting_buffer_context *) (name)->context)->apir_context

#define BUFFER_TO_HOST_HANDLE(name) ((ggml_backend_remoting_buffer_context *) (name)->context)->apir_context.host_handle

#define GET_DEVICE_CONTEXT() (ggml_backend_remoting_device_context *) ggml_backend_remoting_get_device(0)->context

#define BUFT_TO_GPU(name) ((ggml_backend_remoting_device_context *) (name)->device->context)->gpu

struct ggml_backend_remoting_device_context {
    size_t      device;
    std::string name;
    std::string description;

#ifndef GGML_VIRTGPU_USE_WINDOWS
    std::vector<std::tuple<void *, size_t, virtgpu_shmem *>> shared_memory;
    virtgpu * gpu;
#else
    // Windows winApiRmt implementation
    std::vector<std::tuple<void *, size_t, ggml_winapi_shared_buffer_t *>> shared_memory;
    ggml_winapi_handle_t winapi_handle;
    virtgpu * gpu;  // Added for compatibility with ggml backend files
#endif
};

struct ggml_backend_remoting_buffer_context {
    apir_buffer_context_t apir_context;

    virtgpu * gpu;

    void * base;

    bool is_from_ptr;
};

extern const ggml_backend_buffer_type_i ggml_backend_remoting_buffer_type_interface;
extern const ggml_backend_device_i      ggml_backend_remoting_device_interface;
extern const ggml_backend_buffer_i      ggml_backend_remoting_buffer_interface;
extern const ggml_backend_buffer_type_i ggml_backend_remoting_buffer_from_ptr_type_interface;
extern const ggml_backend_buffer_i      ggml_backend_remoting_buffer_from_ptr_interface;

ggml_backend_dev_t         ggml_backend_remoting_get_device(size_t device);
ggml_backend_t             ggml_backend_remoting_device_init(ggml_backend_dev_t dev, const char * params);
ggml_backend_buffer_type_t ggml_backend_remoting_device_get_buffer_type(ggml_backend_dev_t dev);

static inline apir_buffer_type_host_handle_t ggml_buffer_type_to_apir_handle(ggml_backend_buffer_type_t buft) {
    return (apir_buffer_type_host_handle_t) buft;
}

static inline apir_buffer_host_handle_t ggml_buffer_to_apir_handle(ggml_backend_buffer_t buffer) {
    if (!buffer->context) {
        GGML_ABORT("%s: no context available :/", __func__);
    }
    return BUFFER_TO_HOST_HANDLE(buffer);
}
