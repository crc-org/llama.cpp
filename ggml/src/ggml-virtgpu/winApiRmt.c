/*
 * Windows API Remoting Backend Implementation
 *
 * This file implements the Windows backend using winApiRmt for VirtGPU operations.
 * It provides a standalone Windows client implementation that communicates with
 * Windows hosts via TCP and JSON protocol over shared memory.
 */

#include "winApiRmt.h"
#include "./virtgpu-interface.h"
#include "backend/shared/api_remoting.h"
#include "./apir-minimal.h"
#include "ggml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>


/* Forward declarations for static functions */
static int windows_shmem_create(virtgpu* gpu, size_t size, virtgpu_shmem* shmem);
static void windows_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem);

/* Forward declaration for operations table removed - defined at bottom */

/* Buffer sizes - winApiRmt supports dynamic allocation so use larger sizes */
const size_t WINAPI_REPLY_BUFFER_SIZE = 16 * 1024 * 1024;  // 16MB
const size_t WINAPI_DATA_BUFFER_SIZE = 256 * 1024 * 1024;  // 256MB

/* Windows backend-specific data */
typedef struct {
    ggml_winapi_handle_t winapi_handle;
} virtgpu_windows_data;

/* Windows shmem backend data */
typedef struct {
    ggml_winapi_shared_buffer_t buffer;
} virtgpu_windows_shmem_data;

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Windows backend implementation */
static virtgpu* windows_create(void) {
    GGML_LOG_INFO("Initializing Windows virtgpu with winApiRmt transport...\n");

    virtgpu* gpu = (virtgpu*)calloc(1, sizeof(virtgpu));
    if (!gpu) {
        GGML_LOG_ERROR("Failed to allocate virtgpu structure\n");
        return NULL;
    }

    /* Allocate Windows-specific data */
    virtgpu_windows_data* win_data = (virtgpu_windows_data*)calloc(1, sizeof(virtgpu_windows_data));
    if (!win_data) {
        GGML_LOG_ERROR("Failed to allocate Windows backend data\n");
        free(gpu);
        return NULL;
    }

    /* Initialize Windows client connection */
    win_data->winapi_handle = ggml_winapi_init();
    if (!win_data->winapi_handle) {
        GGML_LOG_ERROR("Failed to initialize Windows client connection\n");
        free(win_data);
        free(gpu);
        return NULL;
    }

    gpu->backend_data = win_data;

    /* Initialize utility arrays */
    util_sparse_array_init(&gpu->shmem_array, sizeof(virtgpu_shmem));

    /* Initialize mutex for data buffer synchronization */
    if (mtx_init(&gpu->data_shmem_mutex, mtx_plain) != thrd_success) {
        printf("Failed to initialize data_shmem mutex\n");
        ggml_winapi_cleanup(win_data->winapi_handle);
        free(win_data);
        free(gpu);
        return NULL;
    }

    /* Allocate main communication buffers using direct Windows implementation */
    if (windows_shmem_create(gpu, WINAPI_REPLY_BUFFER_SIZE, &gpu->reply_shmem) != 0) {
        GGML_LOG_ERROR("Failed to allocate reply buffer\n");
        ggml_winapi_cleanup(win_data->winapi_handle);
        free(win_data);
        free(gpu);
        return NULL;
    }

    /* Initialize data_shmem for Linux compatibility (Windows uses dynamic buffers) */
    memset(&gpu->data_shmem, 0, sizeof(gpu->data_shmem));
    gpu->data_shmem.mmap_size = 0;  // Force Linux code to always use dynamic buffers

    /* Create separate command buffer for APIR commands */
    if (windows_shmem_create(gpu, 4096, &gpu->command_shmem) != 0) {
        GGML_LOG_ERROR("Failed to allocate command buffer\n");
        windows_shmem_destroy(gpu, &gpu->reply_shmem);
        ggml_winapi_cleanup(win_data->winapi_handle);
        free(win_data);
        free(gpu);
        return NULL;
    }

    /* Set APIR capabilities */
    gpu->use_apir_capset = getenv("GGML_REMOTING_USE_APIR_CAPSET") != NULL;

    /* Set backend information */
    gpu->backend_type = VIRTGPU_BACKEND_WINDOWS_WINAPI;
    gpu->ops = virtgpu_backend_windows_winapi_get_ops();  // Set ops after structure definition

    GGML_LOG_INFO("Windows initialization complete\n");
    GGML_LOG_INFO("  Reply buffer:   %zu MB\n", WINAPI_REPLY_BUFFER_SIZE / (1024*1024));
    GGML_LOG_INFO("  Command buffer: %zu KB\n", gpu->command_shmem.mmap_size / 1024);
    GGML_LOG_INFO("  Data buffers:   Dynamic allocation\n");

    return gpu;
}

static void windows_destroy(virtgpu* gpu) {
    if (!gpu) {
        return;
    }

    virtgpu_windows_data* win_data = (virtgpu_windows_data*)gpu->backend_data;

    /* Clean up persistent communication buffers */
    virtgpu_shmem_destroy(gpu, &gpu->reply_shmem);
    virtgpu_shmem_destroy(gpu, &gpu->command_shmem);

    /* Clean up utility arrays */
    util_sparse_array_finish(&gpu->shmem_array);

    /* Clean up mutex */
    mtx_destroy(&gpu->data_shmem_mutex);

    /* Clean up Windows connection */
    if (win_data && win_data->winapi_handle) {
        ggml_winapi_cleanup(win_data->winapi_handle);
    }

    if (win_data) {
        free(win_data);
    }

    free(gpu);
    GGML_LOG_INFO("Windows virtgpu cleanup complete\n");
}

static struct apir_encoder* windows_remote_call_prepare(virtgpu* gpu, int apir_cmd_type, int32_t cmd_flags) {

    if (!gpu || !gpu->backend_data) {
        printf("[CLIENT] ERROR: Invalid virtgpu handle in remote_call_prepare\n");
        return NULL;
    }

    /* Use the dedicated shared command buffer to avoid collision with data buffer */
    void* buffer_ptr = virtgpu_shmem_get_ptr(&gpu->command_shmem);
    size_t buffer_size = gpu->command_shmem.mmap_size;

    if (!buffer_ptr || buffer_size == 0) {
        GGML_LOG_ERROR("Invalid data buffer in remote_call_prepare\n");
        return NULL;
    }

    /* Clear the shared command buffer before use to avoid garbage data */
    memset(buffer_ptr, 0, buffer_size);

    /* Create APIR encoder using winApiRmt shared buffer */
    struct apir_encoder* encoder = apir_encoder_init(buffer_ptr, buffer_size);
    if (!encoder) {
        GGML_LOG_ERROR("Failed to initialize APIR encoder\n");
        return NULL;
    }

    /* Encode the command type and flags - same protocol as Linux version */
    apir_encode_uint32_t(encoder, (uint32_t*)&apir_cmd_type);
    apir_encode_int32_t(encoder, &cmd_flags);

    return encoder;
}

static uint32_t windows_remote_call(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder** dec, uint64_t timeout_ms, long long* call_duration_ns) {
    if (!gpu || !gpu->backend_data || !enc || !dec) {
        GGML_LOG_ERROR("Invalid parameters in remote_call\n");
        return APIR_FORWARD_INVALID_ARGUMENT;
    }

    virtgpu_windows_data* win_data = (virtgpu_windows_data*)gpu->backend_data;
    uint64_t start_time = get_time_ns();

    /* Get encoded data size and validate */
    size_t encoded_size = apir_encoder_get_encoded_size(enc);

    if (encoded_size > gpu->command_shmem.mmap_size) {
        GGML_LOG_ERROR("Encoded data size %zu exceeds command buffer size %zu\n",
               encoded_size, gpu->command_shmem.mmap_size);
        return APIR_FORWARD_INVALID_ARGUMENT;
    }

    /* Send via Windows client using JSON protocol - use encoded command data */
    size_t actual_response_size = 0;
    int winapi_ret = ggml_winapi_send_apir_command(win_data->winapi_handle,
                                                  enc->start,
                                                  encoded_size,
                                                  virtgpu_shmem_get_ptr(&gpu->reply_shmem),
                                                  gpu->reply_shmem.mmap_size,
                                                  &actual_response_size);
    if (winapi_ret != GGML_WINAPI_OK) {
        GGML_LOG_ERROR("ggml_winapi_send_apir_command failed with code %d\n", winapi_ret);
        return APIR_FORWARD_HYPERCALL_ERROR;
    }

    /* Response should be in the reply buffer */
    void* reply_ptr = virtgpu_shmem_get_ptr(&gpu->reply_shmem);
    size_t reply_size = gpu->reply_shmem.mmap_size;

    if (!reply_ptr) {
        GGML_LOG_ERROR("Reply buffer is not mapped\n");
        return APIR_FORWARD_HYPERCALL_ERROR;
    }

    /* Initialize decoder with reply buffer */
    *dec = apir_decoder_init(reply_ptr, reply_size);
    if (!*dec) {
        GGML_LOG_ERROR("Failed to initialize APIR decoder\n");
        return APIR_FORWARD_HYPERCALL_ERROR;
    }


    /* Calculate call duration */
    if (call_duration_ns) {
        *call_duration_ns = get_time_ns() - start_time;
    }

    /* Extract return code from response */
    uint32_t return_code = APIR_FORWARD_SUCCESS;
    apir_decode_uint32_t(*dec, &return_code);

    /* Add APIR_FORWARD_BASE_INDEX offset - client expects return codes >= 5 for success */
    return_code += APIR_FORWARD_BASE_INDEX;

    return return_code;

    (void)timeout_ms; // unused parameter
}

static void windows_remote_call_finish(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder* dec) {
    (void)gpu; // gpu not needed for cleanup in Windows implementation

    if (enc) {
        apir_encoder_deinit(enc);
    }

    if (dec) {
        apir_decoder_deinit(dec);
    }
}

static int windows_shmem_create(virtgpu* gpu, size_t size, virtgpu_shmem* shmem) {
    if (!gpu || !gpu->backend_data || !shmem) {
        GGML_LOG_ERROR("Invalid parameters in shmem_create\n");
        return -1;
    }

    virtgpu_windows_data* win_data = (virtgpu_windows_data*)gpu->backend_data;

    /* Allocate Windows-specific shmem data */
    virtgpu_windows_shmem_data* shmem_data = (virtgpu_windows_shmem_data*)malloc(sizeof(virtgpu_windows_shmem_data));
    if (!shmem_data) {
        GGML_LOG_ERROR("Failed to allocate Windows shmem data\n");
        return -1;
    }

    int ret = ggml_winapi_alloc_shared_buffer(win_data->winapi_handle, size, &shmem_data->buffer);
    if (ret != GGML_WINAPI_OK) {
        GGML_LOG_ERROR("Failed to allocate shared buffer of size %zu\n", size);
        free(shmem_data);
        return ret;
    }

    /* Set common fields */
    shmem->res_id = shmem_data->buffer.buffer_id;  // Use buffer_id as res_id for APIR
    shmem->mmap_size = size;
    shmem->mmap_ptr = shmem_data->buffer.data;
    shmem->backend_data = shmem_data;

    /* Register buffer with Windows service */
    printf("Attempting to register buffer %u with Windows service...\n", shmem_data->buffer.buffer_id);
    ret = ggml_winapi_register_buffer(win_data->winapi_handle, &shmem_data->buffer);
    if (ret != GGML_WINAPI_OK) {
        printf("Failed to register buffer %u with Windows service (ret=%d)\n", shmem_data->buffer.buffer_id, ret);
        ggml_winapi_free_shared_buffer(&shmem_data->buffer);
        free(shmem_data);
        return ret;
    }
    printf("Successfully registered buffer %u with Windows service\n", shmem_data->buffer.buffer_id);

    GGML_LOG_INFO("Created shared buffer: %zu bytes at %p, ID=%u\n", size, shmem->mmap_ptr, shmem->res_id);
    return 0;
}

static void windows_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem) {
    (void)gpu; // unused in Windows implementation
    if (shmem && shmem->backend_data) {
        GGML_LOG_INFO("Destroying shared buffer: %zu bytes\n", shmem->mmap_size);

        virtgpu_windows_shmem_data* shmem_data = (virtgpu_windows_shmem_data*)shmem->backend_data;
        ggml_winapi_free_shared_buffer(&shmem_data->buffer);
        free(shmem_data);

        memset(shmem, 0, sizeof(*shmem));
    }
}

static void* windows_shmem_get_ptr(virtgpu_shmem* shmem) {
    return shmem ? shmem->mmap_ptr : NULL;
}

/* Windows backend operations table */
static const virtgpu_backend_ops windows_ops = {
    .name = "Windows WinAPI",
    .create = windows_create,
    .destroy = windows_destroy,
    .remote_call_prepare = windows_remote_call_prepare,
    .remote_call = windows_remote_call,
    .remote_call_finish = windows_remote_call_finish,
    .shmem_create = windows_shmem_create,
    .shmem_destroy = windows_shmem_destroy,
    .shmem_get_ptr = windows_shmem_get_ptr,
    .sparse_array_init = util_sparse_array_init,
    .sparse_array_finish = util_sparse_array_finish,
    .sparse_array_get = util_sparse_array_get,
    .sparse_array_set = util_sparse_array_set,
};

/* Public interface */
const virtgpu_backend_ops* virtgpu_backend_windows_winapi_get_ops(void) {
    return &windows_ops;
}

