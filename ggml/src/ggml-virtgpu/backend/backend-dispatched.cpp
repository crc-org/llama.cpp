#include "backend-dispatched.h"
#include "backend-virgl-apir.h"

#include "ggml-backend-impl.h"
#include "ggml-backend.h"
#include "ggml-impl.h"

#include <cstdint>

ggml_backend_reg_t reg = NULL;
ggml_backend_dev_t dev = NULL;
ggml_backend_t     bck = NULL;

uint64_t timer_start = 0;
uint64_t timer_total = 0;
uint64_t timer_count = 0;

uint32_t backend_dispatch_initialize(void * ggml_backend_reg_fct_p) {
    printf("[BACKEND_INIT] backend_dispatch_initialize called with function pointer: %p\n", ggml_backend_reg_fct_p);
    printf("[BACKEND_INIT] Initial state: reg=%p, dev=%p, bck=%p\n", (void*)reg, (void*)dev, (void*)bck);

    if (reg != NULL) {
        printf("[BACKEND_INIT] Backend already initialized\n");
        GGML_LOG_WARN("%s: already initialized\n", __func__);
        return APIR_BACKEND_INITIALIZE_ALREADY_INITED;
    }

    ggml_backend_reg_t (*ggml_backend_reg_fct)(void) = (ggml_backend_reg_t (*)()) ggml_backend_reg_fct_p;
    printf("[BACKEND_INIT] Calling registration function...\n");

    reg = ggml_backend_reg_fct();
    printf("[BACKEND_INIT] Registration function returned: reg=%p\n", (void*)reg);

    if (reg == NULL) {
        printf("[BACKEND_INIT] ERROR: Backend registration failed\n");
        GGML_LOG_ERROR("%s: backend registration failed\n", __func__);
        return APIR_BACKEND_INITIALIZE_BACKEND_REG_FAILED;
    }

    printf("[BACKEND_INIT] Getting device count...\n");
    int device_count = reg->iface.get_device_count(reg);
    printf("[BACKEND_INIT] Device count: %d\n", device_count);

    if (!device_count) {
        printf("[BACKEND_INIT] ERROR: No devices found\n");
        GGML_LOG_ERROR("%s: backend initialization failed: no device found\n", __func__);
        return APIR_BACKEND_INITIALIZE_NO_DEVICE;
    }

    printf("[BACKEND_INIT] Getting device 0...\n");
    dev = reg->iface.get_device(reg, 0);
    printf("[BACKEND_INIT] Got device: dev=%p\n", (void*)dev);

    if (!dev) {
        printf("[BACKEND_INIT] ERROR: Failed to get device 0\n");
        GGML_LOG_ERROR("%s: backend initialization failed: no device received\n", __func__);
        return APIR_BACKEND_INITIALIZE_NO_DEVICE;
    }

    printf("[BACKEND_INIT] Initializing backend...\n");
    bck = dev->iface.init_backend(dev, NULL);
    printf("[BACKEND_INIT] Backend initialized: bck=%p\n", (void*)bck);

    printf("[BACKEND_INIT] SUCCESS - Final state: reg=%p, dev=%p, bck=%p\n", (void*)reg, (void*)dev, (void*)bck);
    return APIR_BACKEND_INITIALIZE_SUCCESS;
}
