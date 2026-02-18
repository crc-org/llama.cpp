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
    if (reg != NULL) {
        GGML_LOG_WARN(GGML_VIRTGPU_BCK "%s: already initialized\n", __func__);
        return APIR_BACKEND_INITIALIZE_ALREADY_INITED;
    }

    // Note: The actual initialization work is now done by the frontend
    // Frontend calls apir_backend_initialize() -> backend_backend_initialize()
    // This function just checks if initialization was completed by frontend

    if (bck != NULL) {
        // Frontend already completed initialization
        return APIR_BACKEND_INITIALIZE_SUCCESS;
    }

    // Frontend hasn't initialized yet - this shouldn't happen in normal flow
    GGML_LOG_ERROR(GGML_VIRTGPU_BCK "%s: Backend not initialized by frontend\n", __func__);
    return APIR_BACKEND_INITIALIZE_BACKEND_INIT_FAILED;
}
