#pragma once

// clang-format off
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <mutex>

#include <ggml-backend.h>

#include "backend-convert.h"
#include "backend-virgl-apir.h"
#include "shared/apir_backend.h"
#include "shared/apir_cs.h"
#include "shared/apir_cs_ggml.h"
// clang-format on

#define GGML_VIRTGPU_BCK "ggml-virtgpu-backend: "

struct virgl_apir_context {
    uint32_t               ctx_id;
    virgl_apir_callbacks * iface;
};

typedef uint32_t (*backend_dispatch_t)(apir_encoder * enc, apir_decoder * dec, virgl_apir_context * ctx);

#include "backend-dispatched.gen.h"

// Backend instance structure - one backend per instance
struct apir_backend_instance {
    ggml_backend_t bck;    // The actual backend
    uint32_t       magic;  // For validation: 0xAB1234CD
};

// Device context structure - can have multiple backend instances
struct apir_device_context {
    std::mutex                                             backends_mutex;
    std::unordered_map<uintptr_t, apir_backend_instance *> backend_instances;
    uintptr_t                                              next_backend_id;

    bool async_backend;  // Whether the backend supports async operations

    uint32_t magic;      // For validation: 0xAB1234CD
};

#define APIR_DEVICE_EXTENSION_MAGIC 0xAB1234CD
#define APIR_BACKEND_INSTANCE_MAGIC 0xCD4321BA

// Device context management
apir_device_context * get_device_context(ggml_backend_dev_t dev);
void                    ensure_device_context(ggml_backend_dev_t dev);
void                    cleanup_device_context(ggml_backend_dev_t dev);

// Backend instance management
uintptr_t               create_backend_instance(ggml_backend_dev_t dev);
apir_backend_instance * get_backend_instance(ggml_backend_dev_t dev, uintptr_t backend_id);
void                    cleanup_backend_instance(ggml_backend_dev_t dev, uintptr_t backend_id);

uint32_t backend_dispatch_initialize(void * ggml_backend_reg_fct_p, uintptr_t * out_handle, uint32_t * out_backend_id);
