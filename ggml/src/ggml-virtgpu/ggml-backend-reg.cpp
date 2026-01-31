#include "ggml-remoting.h"
#include "../../include/ggml-virtgpu.h"

#include <iostream>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

void ggml_virtgpu_cleanup(virtgpu *gpu);

static virtgpu * apir_initialize() {
    static virtgpu *         gpu          = NULL;
    static bool initialized  = false;

    if (initialized) {
        // fast track
        return gpu;
    }

    {
        static std::mutex           mutex;
        std::lock_guard<std::mutex> lock(mutex);

        if (initialized) {
            // thread safe
            return gpu;
        }

        gpu = create_virtgpu();
        if (!gpu) {
            printf("failed to initialize the virtgpu\n");
            _exit(1);
        }

        printf("\n[VIRTGPU] Populating the cache values\n");
        // Pre-fetch and cache all device information, it will not change
        gpu->cached_device_info.description  = apir_device_get_description(gpu);
        if (!gpu->cached_device_info.description) {
            GGML_ABORT("failed to initialize the virtgpu device description");
        }
        printf("==> %s\n", gpu->cached_device_info.description);

        gpu->cached_device_info.name         = apir_device_get_name(gpu);
        if (!gpu->cached_device_info.name) {
            GGML_ABORT("failed to initialize the virtgpu device name");
        }
        printf("==> %s\n", gpu->cached_device_info.name);
        gpu->cached_device_info.device_count = apir_device_get_count(gpu);
        printf("==> %d\n", gpu->cached_device_info.device_count);
        gpu->cached_device_info.type         = apir_device_get_type(gpu);

        apir_device_get_memory(gpu,
                              &gpu->cached_device_info.memory_free,
                              &gpu->cached_device_info.memory_total);

        apir_buffer_type_host_handle_t buft_host_handle = apir_device_get_buffer_type(gpu);
        if (buft_host_handle == 0) {
            GGML_ABORT("failed to get valid buffer type from device - Windows service returned garbage data");
        }
        gpu->cached_buffer_type.host_handle             = buft_host_handle;
        gpu->cached_buffer_type.name                    = apir_buffer_type_get_name(gpu, buft_host_handle);
        if (!gpu->cached_buffer_type.name) {
            GGML_ABORT("failed to initialize the virtgpu buffer type name");
        }
        gpu->cached_buffer_type.alignment               = apir_buffer_type_get_alignment(gpu, buft_host_handle);
        gpu->cached_buffer_type.max_size                = apir_buffer_type_get_max_size(gpu, buft_host_handle);
        printf("[VIRTGPU] Populating the cache values ==> Done\n");
        initialized = true;
    }

    return gpu;
}

static int ggml_backend_remoting_get_device_count() {
    virtgpu * gpu = apir_initialize();
    if (!gpu) {
        fprintf(stderr, "apir_initialize failed\n");
        return 0;
    }

    int device_count = gpu->cached_device_info.device_count;

    return device_count;
}

static size_t ggml_backend_remoting_reg_get_device_count(ggml_backend_reg_t reg) {
    UNUSED(reg);

    return ggml_backend_remoting_get_device_count();
}

static std::vector<ggml_backend_dev_t> devices;

ggml_backend_dev_t ggml_backend_remoting_get_device(size_t device) {
    GGML_ASSERT(device < devices.size());
    return devices[device];
}

static void ggml_backend_remoting_reg_init_devices(ggml_backend_reg_t reg) {
    if (devices.size() > 0) {
        printf("%s: already initialized\n", __func__);
        return;
    }

    virtgpu * gpu = apir_initialize();
    if (!gpu) {
        fprintf(stderr, "apir_initialize failed\n");
        return;
    }

    static bool initialized = false;

    if (initialized) {
        return; // fast track
    }

    {
        static std::mutex           mutex;
        std::lock_guard<std::mutex> lock(mutex);
        if (!initialized) {
            for (int i = 0; i < ggml_backend_remoting_get_device_count(); i++) {
                ggml_backend_remoting_device_context * ctx       = new ggml_backend_remoting_device_context;
                char                                   desc[256] = "API Remoting device";

                ctx->device      = i;
                ctx->name        = std::string(GGML_REMOTING_FRONTEND_NAME) + std::to_string(i);
                ctx->description = std::string(desc);
                ctx->gpu         = gpu;

                ggml_backend_dev_t dev = new ggml_backend_device{
                    /* .iface   = */ ggml_backend_remoting_device_interface,
                    /* .reg     = */ reg,
                    /* .context = */ ctx,
                };
                devices.push_back(dev);
            }
            initialized = true;
        }
    }
}

static ggml_backend_dev_t ggml_backend_remoting_reg_get_device(ggml_backend_reg_t reg, size_t device) {
    UNUSED(reg);

    return ggml_backend_remoting_get_device(device);
}

static const char * ggml_backend_remoting_reg_get_name(ggml_backend_reg_t reg) {
    UNUSED(reg);

    return GGML_REMOTING_FRONTEND_NAME;
}

static const ggml_backend_reg_i ggml_backend_remoting_reg_i = {
    /* .get_name         = */ ggml_backend_remoting_reg_get_name,
    /* .get_device_count = */ ggml_backend_remoting_reg_get_device_count,
    /* .get_device       = */ ggml_backend_remoting_reg_get_device,
    /* .get_proc_address = */ NULL,
};

ggml_backend_reg_t ggml_backend_virtgpu_reg() {
    virtgpu * gpu = apir_initialize();
    if (!gpu) {
        fprintf(stderr, "virtgpu_apir_initialize failed\n");
        return NULL;
    }

    static ggml_backend_reg reg = {
        /* .api_version = */ GGML_BACKEND_API_VERSION,
        /* .iface       = */ ggml_backend_remoting_reg_i,
        /* .context     = */ gpu,
    };

    static bool initialized = false;
    if (initialized) {
        return &reg;
    }
    initialized = true;

    ggml_backend_remoting_reg_init_devices(&reg);

    printf("%s: initialized\n", __func__);

    return &reg;
}

GGML_BACKEND_DL_IMPL(ggml_backend_virtgpu_reg)

// public function, not exposed in the GGML interface at the moment
void ggml_virtgpu_cleanup(virtgpu *gpu) {
    if (gpu->cached_device_info.name) {
        free(gpu->cached_device_info.name);
        gpu->cached_device_info.name = NULL;
    }
    if (gpu->cached_device_info.description) {
        free(gpu->cached_device_info.description);
        gpu->cached_device_info.description = NULL;
    }
    if (gpu->cached_buffer_type.name) {
        free(gpu->cached_buffer_type.name);
        gpu->cached_buffer_type.name = NULL;
    }
    mtx_destroy(&gpu->data_shmem_mutex);
}
