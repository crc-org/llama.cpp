#ifdef GGML_VIRTGPU_USE_WINDOWS
#include "virtgpu-interface.h"
#include <threads.h>  // For mtx_t, mtx_lock, mtx_unlock
#else
#include "virtgpu.h"
#endif

#include "ggml-remoting.h"
#include "backend/shared/apir_backend.h"
#include "backend/shared/apir_cs_ggml.h"
#include "backend/shared/api_remoting.h"

#include "../ggml-backend-impl.h"

/* Function name mapping for client-side logging */
static inline const char * frontend_command_name(int cmd_type) {
    switch (cmd_type) {
        case 0: return "backend_device_get_device_count";
        case 1: return "backend_device_get_count";
        case 2: return "backend_device_get_name";
        case 3: return "backend_device_get_description";
        case 4: return "backend_device_get_type";
        case 5: return "backend_device_get_memory";
        case 6: return "backend_device_supports_op";
        case 7: return "backend_device_get_buffer_type";
        case 8: return "backend_device_get_props";
        case 9: return "backend_device_buffer_from_ptr";
        case 10: return "backend_buffer_type_get_name";
        case 11: return "backend_buffer_type_get_alignment";
        case 12: return "backend_buffer_type_get_max_size";
        case 13: return "backend_buffer_type_is_host";
        case 14: return "backend_buffer_type_alloc_buffer";
        case 15: return "backend_buffer_type_get_alloc_size";
        case 16: return "backend_buffer_get_base";
        case 17: return "backend_buffer_set_tensor";
        case 18: return "backend_buffer_get_tensor";
        case 19: return "backend_buffer_cpy_tensor";
        case 20: return "backend_buffer_clear";
        case 21: return "backend_buffer_free_buffer";
        case 22: return "backend_backend_graph_compute";
        default: return "UNKNOWN";
    }
}

#define REMOTE_CALL_PREPARE(gpu_dev_name, encoder_name, apir_command_type__)                               \
    do {                                                                                                   \
        int32_t forward_flag = (int32_t) apir_command_type__;                                              \
        encoder_name         = remote_call_prepare(gpu_dev_name, APIR_COMMAND_TYPE_FORWARD, forward_flag); \
        if (!encoder_name) {                                                                               \
            printf("FATAL: %s: failed to prepare the remote call encoder\n", __func__);                 \
            fflush(stdout);                                                                              \
            exit(1);                                                                                     \
        }                                                                                                  \
    } while (0)

#define REMOTE_CALL(gpu_dev_name, encoder_name, decoder_name, ret_name)                                           \
    do {                                                                                                          \
        ret_name = (ApirForwardReturnCode) remote_call(gpu_dev_name, encoder_name, &decoder_name, 0, NULL);       \
        if (!decoder_name) {                                                                                      \
            printf("FATAL: %s: failed to kick the remote call\n", __func__);                                   \
            fflush(stdout);                                                                                      \
            exit(1);                                                                                             \
        }                                                                                                         \
        if (ret_name < APIR_FORWARD_BASE_INDEX) {                                                                 \
            printf("FATAL: %s: failed to forward the API call: %s: code %d\n", __func__,                       \
                   apir_forward_error(ret_name), ret_name);                                                       \
            fflush(stdout);                                                                                        \
            exit(1);                                                                                               \
        }                                                                                                         \
        ret_name = (ApirForwardReturnCode) (ret_name - APIR_FORWARD_BASE_INDEX);                                  \
    } while (0)
