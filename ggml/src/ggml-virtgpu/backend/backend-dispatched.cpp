#include "backend-dispatched.h"

#include "backend-virgl-apir.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

// Global variables for device functions
ggml_backend_reg_t reg = NULL;
ggml_backend_dev_t dev = NULL;

// Device context management
static std::unordered_map<ggml_backend_dev_t, apir_device_context *> device_contexts;
static std::mutex                                                      device_contexts_mutex;

uint64_t timer_start = 0;
uint64_t timer_total = 0;
uint64_t timer_count = 0;

// Get device context (device-owned backend instances)
apir_device_context * get_device_context(ggml_backend_dev_t device) {
    std::lock_guard<std::mutex> lock(device_contexts_mutex);
    auto                        it = device_contexts.find(device);
    if (it == device_contexts.end()) {
        return nullptr;
    }
    apir_device_context * ext = it->second;
    if (ext->magic != APIR_DEVICE_EXTENSION_MAGIC) {
        return nullptr;
    }
    return ext;
}

// Ensure device context exists
void ensure_device_context(ggml_backend_dev_t device) {
    std::lock_guard<std::mutex> lock(device_contexts_mutex);

    auto it = device_contexts.find(device);
    if (it == device_contexts.end()) {
        apir_device_context * ext = new apir_device_context();
        ext->next_backend_id        = 1;

        // Get async backend properties from the device
        ggml_backend_dev_props props;
        device->iface.get_props(device, &props);
        ext->async_backend = props.caps.async;

        ext->magic                = APIR_DEVICE_EXTENSION_MAGIC;
        device_contexts[device] = ext;
    }
}

// Create new backend instance for device
uintptr_t create_backend_instance(ggml_backend_dev_t device) {
    ensure_device_context(device);
    apir_device_context * ext = get_device_context(device);
    if (ext == nullptr) {
        return 0;  // Failed
    }

    std::lock_guard<std::mutex> lock(ext->backends_mutex);

    apir_backend_instance * instance = new apir_backend_instance();
    instance->bck                    = device->iface.init_backend(device, NULL);
    instance->magic                  = APIR_BACKEND_INSTANCE_MAGIC;

    if (instance->bck == nullptr) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: device->iface.init_backend failed for device %p\n", __func__,
                       (void *) device);
        delete instance;
        return 0;  // Failed
    }

    uintptr_t backend_id               = ext->next_backend_id++;
    ext->backend_instances[backend_id] = instance;

    return backend_id;
}

// Get backend instance
apir_backend_instance * get_backend_instance(ggml_backend_dev_t device, uintptr_t backend_id) {
    apir_device_context * ext = get_device_context(device);
    if (ext == nullptr) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(ext->backends_mutex);
    auto                        it = ext->backend_instances.find(backend_id);
    if (it == ext->backend_instances.end()) {
        return nullptr;
    }

    apir_backend_instance * instance = it->second;
    if (instance->magic != APIR_BACKEND_INSTANCE_MAGIC) {
        return nullptr;
    }

    return instance;
}

// Cleanup specific backend instance
void cleanup_backend_instance(ggml_backend_dev_t device, uintptr_t backend_id) {
    apir_device_context * ext = get_device_context(device);
    if (ext == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(ext->backends_mutex);
    auto                        it = ext->backend_instances.find(backend_id);
    if (it != ext->backend_instances.end()) {
        apir_backend_instance * instance = it->second;

        // Free backend
        if (instance->bck) {
            ggml_backend_free(instance->bck);
            instance->bck = nullptr;
        }

        instance->magic = 0;  // Invalidate
        delete instance;
        ext->backend_instances.erase(it);
    }
}

// Cleanup device context and all its backend instances
void cleanup_device_context(ggml_backend_dev_t device) {
    std::lock_guard<std::mutex> lock(device_contexts_mutex);

    auto it = device_contexts.find(device);
    if (it != device_contexts.end()) {
        apir_device_context * ext = it->second;

        // Clean up all backend instances
        {
            std::lock_guard<std::mutex> backends_lock(ext->backends_mutex);
            for (auto & [backend_id, instance] : ext->backend_instances) {
                if (instance->bck) {
                    ggml_backend_free(instance->bck);
                }

                instance->magic = 0;
                delete instance;
            }
            ext->backend_instances.clear();
        }

        ext->magic = 0;  // Invalidate
        delete ext;
        device_contexts.erase(it);
    }
}

uint32_t backend_dispatch_initialize(void * ggml_backend_reg_fct_p, uintptr_t * out_handle, uint32_t * out_backend_id) {
    GGML_UNUSED(ggml_backend_reg_fct_p);  // reg/dev are already set during library loading

    if (out_handle == nullptr || out_backend_id == nullptr) {
        return APIR_BACKEND_INITIALIZE_BACKEND_INIT_FAILED;
    }

    // Ensure global variables are set (should be done during library loading)
    if (reg == NULL || dev == NULL) {
        GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Global reg/dev not initialized (reg=%p, dev=%p)\n", __func__, (void *) reg,
                       (void *) dev);
        return APIR_BACKEND_INITIALIZE_BACKEND_INIT_FAILED;
    }

    // Create new backend instance
    uintptr_t backend_id = create_backend_instance(dev);
    if (backend_id == 0) {
        return APIR_BACKEND_INITIALIZE_BACKEND_INIT_FAILED;
    }

    // Set output parameters
    *out_handle     = (uintptr_t) dev;
    *out_backend_id = (uint32_t) backend_id;

    return APIR_BACKEND_INITIALIZE_SUCCESS;
}
