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
#include "ggml-impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>  // For madvise and MADV_DONTNEED
#include <fcntl.h>     // For open()
#include <stdatomic.h> // For atomic operations
#include <errno.h>     // For errno
#include <sys/socket.h> // For send(), recv()
#include <netinet/in.h> // For ntohl(), htonl()
#include <signal.h>     // For signal handlers
#include <execinfo.h>   // For backtrace

/* DEBUG: First write detection */
void* debug_protected_buffer = NULL;
size_t debug_protected_size = 0;
static bool debug_first_write_caught = false;

/* Function name mapping for client-side logging */
static const char * frontend_command_name(int cmd_type) {
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

/* Temporary file management for per-request communication */
typedef struct {
    char file_path[256];
    int fd;
    void* mmap_ptr;
    size_t size;
} temp_file_buffer_t;

/* Result of temporary file request */
typedef struct {
    enum ApirForwardReturnCode status;
    size_t actual_response_size;
} temp_file_request_result_t;

/* Create and mmap a temporary file */
static inline int create_temp_file_buffer(temp_file_buffer_t* buf, const char* prefix, size_t size) {
    static uint32_t temp_counter = 0;

    /* Generate unique temporary file path */
    snprintf(buf->file_path, sizeof(buf->file_path),
             "/mnt/c/temp/ggml_temp_%s_%u_%zu.dat",
             prefix, __atomic_fetch_add(&temp_counter, 1, __ATOMIC_SEQ_CST), size);

    /* Create the temporary file */
    buf->fd = open(buf->file_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (buf->fd < 0) {
        printf("Failed to create temp file %s: %s\n", buf->file_path, strerror(errno));
        return -1;
    }

    /* Resize file to requested size */
    if (ftruncate(buf->fd, size) != 0) {
        printf("Failed to resize temp file: %s\n", strerror(errno));
        close(buf->fd);
        unlink(buf->file_path);
        return -1;
    }

    /* Map the file into memory */
    buf->mmap_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buf->fd, 0);
    if (buf->mmap_ptr == MAP_FAILED) {
        printf("Failed to mmap temp file: %s\n", strerror(errno));
        close(buf->fd);
        unlink(buf->file_path);
        return -1;
    }

    buf->size = size;
    return 0;
}

/* Cleanup temporary file buffer */
static inline void cleanup_temp_file_buffer(temp_file_buffer_t* buf) {
    if (buf->mmap_ptr && buf->mmap_ptr != MAP_FAILED) {
        munmap(buf->mmap_ptr, buf->size);
        buf->mmap_ptr = NULL;
    }
    if (buf->fd >= 0) {
        close(buf->fd);
        buf->fd = -1;
    }
    if (buf->file_path[0]) {
        unlink(buf->file_path);
        buf->file_path[0] = '\0';
    }
}

/* Receive JSON response from Windows service */
static inline int receive_json_response(int sockfd, char* response_buffer, size_t buffer_size) {
    /* Receive length header first */
    uint32_t network_len;
    ssize_t recv_header = recv(sockfd, (char*)&network_len, sizeof(network_len), 0);
    if (recv_header != sizeof(network_len)) {
        printf("Failed to receive response header: %s\n", strerror(errno));
        return -1;
    }

    uint32_t msg_len = ntohl(network_len);
    if (msg_len >= buffer_size) {
        printf("Response too large: %u bytes, buffer only %zu\n", msg_len, buffer_size);
        return -1;
    }

    /* Receive JSON data */
    ssize_t recv_data = recv(sockfd, response_buffer, msg_len, 0);
    if (recv_data != (ssize_t)msg_len) {
        printf("Failed to receive response data: %s\n", strerror(errno));
        return -1;
    }

    response_buffer[msg_len] = '\0';
    return 0;
}

/* Send temporary file request to Windows service */
static inline temp_file_request_result_t send_temp_file_request(virtgpu* gpu, const char* cmd_file_path, const char* reply_file_path, uint32_t cmd_type, size_t cmd_size) {
#ifdef GGML_VIRTGPU_USE_WINDOWS
    /* Access Windows backend data */
    typedef struct {
        ggml_winapi_handle_t winapi_handle;
    } virtgpu_windows_data;

    typedef struct {
        int socket_fd;
        uint32_t next_buffer_id;
        char shared_memory_base[256];
        /* Other fields not needed here */
    } ggml_winapi_context_t;

    virtgpu_windows_data* win_data = (virtgpu_windows_data*)gpu->backend_data;
    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)win_data->winapi_handle;

    /* Create JSON request for Windows service */
    char json_request[1024];
    snprintf(json_request, sizeof(json_request),
             "{"
             "\"api\":\"apir\","
             "\"request_id\":1,"
             "\"apir_cmd_type\":%u,"
             "\"apir_data_size\":%zu,"
             "\"shared_file_path\":\"%s\","
             "\"response_file_path\":\"%s\","
             "\"buffer_id\":1,"
             "\"response_buffer_id\":2"
             "}",
             cmd_type, cmd_size, cmd_file_path, reply_file_path);

    /* Send JSON via TCP socket */
    size_t msg_len = strlen(json_request);
    uint32_t network_len = htonl((uint32_t)msg_len);

    /* Send length header first */
    ssize_t sent_header = send(ctx->socket_fd, (char*)&network_len, sizeof(network_len), 0);
    if (sent_header != sizeof(network_len)) {
        printf("Failed to send request header: %s\n", strerror(errno));
        temp_file_request_result_t error_result = {APIR_FORWARD_HYPERCALL_ERROR, 0};
        return error_result;
    }

    /* Send JSON data */
    ssize_t sent_data = send(ctx->socket_fd, json_request, msg_len, 0);
    if (sent_data != (ssize_t)msg_len) {
        printf("Failed to send request data: %s\n", strerror(errno));
        temp_file_request_result_t error_result = {APIR_FORWARD_HYPERCALL_ERROR, 0};
        return error_result;
    }

    /* Receive response */
    char response_buffer[2048];
    if (receive_json_response(ctx->socket_fd, response_buffer, sizeof(response_buffer)) != 0) {
        printf("Failed to receive JSON response\n");
        temp_file_request_result_t error_result = {APIR_FORWARD_HYPERCALL_ERROR, 0};
        return error_result;
    }

    /* Parse response_size from JSON - simple string search */
    size_t actual_response_size = 0;
    const char* size_start = strstr(response_buffer, "\"response_size\":");
    if (size_start) {
        size_start += 16; // Skip "response_size":
        actual_response_size = (size_t)strtoul(size_start, NULL, 10);
    }

    /* Return both success code and actual size */
    temp_file_request_result_t success_result = {
        (enum ApirForwardReturnCode)(APIR_FORWARD_BASE_INDEX + 0),
        actual_response_size
    };
    return success_result;
#else
    /* Suppress unused parameter warnings */
    (void)gpu;
    (void)cmd_file_path;
    (void)reply_file_path;
    (void)cmd_type;
    (void)cmd_size;
    temp_file_request_result_t error_result = {APIR_FORWARD_HYPERCALL_ERROR, 0};
    return error_result;
#endif
}

/* Forward declarations for static functions */
static int windows_shmem_create(virtgpu* gpu, size_t size, virtgpu_shmem* shmem);
static void windows_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem);

/* Forward declaration for operations table removed - defined at bottom */

/* Buffer sizes - winApiRmt supports dynamic allocation so use larger sizes */
const size_t WINAPI_REPLY_BUFFER_SIZE = 16 * 1024 * 1024;  // 16MB
const size_t WINAPI_DATA_BUFFER_SIZE = 256 * 1024 * 1024;  // 256MB

/* Per-request temporary files for Windows backend */
typedef struct {
    char cmd_file_path[256];
    char reply_file_path[256];
    void* temp_cmd_data;
    size_t temp_cmd_data_size;
} virtgpu_temp_request;

/* Windows backend-specific data */
typedef struct {
    ggml_winapi_handle_t winapi_handle;
    virtgpu_temp_request temp_request;
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

    /* Register persistent buffers with Windows API client for consistent usage */
    virtgpu_windows_shmem_data* reply_data = (virtgpu_windows_shmem_data*)gpu->reply_shmem.backend_data;
    virtgpu_windows_shmem_data* command_data = (virtgpu_windows_shmem_data*)gpu->command_shmem.backend_data;

    int buffer_reg_ret = ggml_winapi_set_apir_buffers(win_data->winapi_handle,
                                                      &reply_data->buffer,
                                                      &command_data->buffer);
    if (buffer_reg_ret != GGML_WINAPI_OK) {
        fprintf(stderr, "Failed to register persistent APIR buffers with client\n");
        windows_shmem_destroy(gpu, &gpu->command_shmem);
        windows_shmem_destroy(gpu, &gpu->reply_shmem);
        ggml_winapi_cleanup(win_data->winapi_handle);
        free(win_data);
        free(gpu);
        return NULL;
    }

    /* Set backend information */
    gpu->backend_type = VIRTGPU_BACKEND_WINDOWS_WINAPI;
    gpu->ops = virtgpu_backend_windows_winapi_get_ops();  // Set ops after structure definition

    printf("Windows initialization complete\n");
    printf("  Reply buffer:   %zu MB (ID=%u, file=%s)\n",
           WINAPI_REPLY_BUFFER_SIZE / (1024*1024),
           reply_data->buffer.buffer_id,
           reply_data->buffer.file_path);
    printf("  Command buffer: %zu KB (ID=%u, file=%s)\n",
           gpu->command_shmem.mmap_size / 1024,
           command_data->buffer.buffer_id,
           command_data->buffer.file_path);
    printf("  Data buffers:   Dynamic allocation\n");

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
    static uint32_t temp_counter = 0;

    if (!gpu || !gpu->backend_data) {
        printf("[CLIENT] ERROR: Invalid virtgpu handle in remote_call_prepare\n");
        return NULL;
    }

    // printf("[FRONTEND_DISPATCHER] Calling method: %s (cmd_type=%d)\n", frontend_command_name(cmd_flags), cmd_flags);

    virtgpu_windows_data* win_data = (virtgpu_windows_data*)gpu->backend_data;

    /* Generate unique temporary file paths */
    uint32_t counter = __atomic_fetch_add(&temp_counter, 1, __ATOMIC_SEQ_CST);
    snprintf(win_data->temp_request.cmd_file_path, sizeof(win_data->temp_request.cmd_file_path),
             "/mnt/c/temp/ggml_temp_cmd_win_%u.dat", counter);
    snprintf(win_data->temp_request.reply_file_path, sizeof(win_data->temp_request.reply_file_path),
             "/mnt/c/temp/ggml_temp_reply_win_%u.dat", counter);

    /* Allocate temporary buffer for encoding */
    size_t buffer_size = 4096;  // 4KB for command buffer
    win_data->temp_request.temp_cmd_data = malloc(buffer_size);
    if (!win_data->temp_request.temp_cmd_data) {
        printf("Failed to allocate temporary command buffer\n");
        return NULL;
    }
    win_data->temp_request.temp_cmd_data_size = buffer_size;

    /* Clear the buffer */
    memset(win_data->temp_request.temp_cmd_data, 0, buffer_size);

    /* Create APIR encoder using temporary buffer */
    struct apir_encoder* encoder = apir_encoder_init(win_data->temp_request.temp_cmd_data, buffer_size);
    if (!encoder) {
        printf("Failed to initialize APIR encoder\n");
        free(win_data->temp_request.temp_cmd_data);
        win_data->temp_request.temp_cmd_data = NULL;
        return NULL;
    }

    /* Encode the command type and flags - same protocol as Linux version */
    apir_encode_uint32_t(encoder, (uint32_t*)&apir_cmd_type);
    apir_encode_int32_t(encoder, &cmd_flags);

    return encoder;
}

static uint32_t windows_remote_call(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder** dec, uint64_t timeout_ms, long long* call_duration_ns) {
    (void)timeout_ms; // unused parameter
    (void)call_duration_ns; // unused parameter

    if (!gpu || !enc || !dec) {
        printf("Invalid parameters in remote_call\n");
        return APIR_FORWARD_INVALID_ARGUMENT;
    }

    // Use the working implementation from execute_temp_file_remote_call
    temp_file_buffer_t temp_cmd_buf;
    temp_file_buffer_t temp_reply_buf;
    ApirForwardReturnCode result;
    uint32_t actual_cmd_type = 0;

    /* Initialize structures */
    memset(&temp_cmd_buf, 0, sizeof(temp_cmd_buf));
    memset(&temp_reply_buf, 0, sizeof(temp_reply_buf));
    temp_cmd_buf.fd = -1;
    temp_reply_buf.fd = -1;

    size_t cmd_size = enc->cur - enc->start;
    size_t reply_size = 16 * 1024 * 1024; /* 16MB reply buffer */

    /* Create and populate command file */
    if (create_temp_file_buffer(&temp_cmd_buf, "cmd", cmd_size) != 0) {
        printf("Failed to create temp command file\n");
        *dec = NULL;
        return APIR_FORWARD_HYPERCALL_ERROR;
    }

    memcpy(temp_cmd_buf.mmap_ptr, enc->start, cmd_size);

    /* Force data to disk to ensure WSL2/Windows cache coherency */
    msync(temp_cmd_buf.mmap_ptr, temp_cmd_buf.size, MS_SYNC);
    fsync(temp_cmd_buf.fd);

    munmap(temp_cmd_buf.mmap_ptr, temp_cmd_buf.size);
    close(temp_cmd_buf.fd);
    temp_cmd_buf.mmap_ptr = NULL;
    temp_cmd_buf.fd = -1;

    /* Create empty reply file */
    if (create_temp_file_buffer(&temp_reply_buf, "reply", reply_size) != 0) {
        printf("Failed to create temp reply file\n");
        cleanup_temp_file_buffer(&temp_cmd_buf);
        *dec = NULL;
        return APIR_FORWARD_HYPERCALL_ERROR;
    }

    munmap(temp_reply_buf.mmap_ptr, temp_reply_buf.size);
    close(temp_reply_buf.fd);
    temp_reply_buf.mmap_ptr = NULL;
    temp_reply_buf.fd = -1;

    /* Extract function_id from APIR structure for JSON transmission */
    if (cmd_size >= 8) {
        const uint32_t* data_ptr = (const uint32_t*)enc->start;
        /* APIR data structure: [uint32_t apir_cmd_type, int32_t function_id, ...] */
        uint32_t function_id = data_ptr[1];
        /* Use the function_id for JSON (not apir_cmd_type) */
        actual_cmd_type = function_id;
    }

    /* Send request to Windows service with temporary file paths */
    temp_file_request_result_t request_result = send_temp_file_request(gpu, temp_cmd_buf.file_path, temp_reply_buf.file_path, actual_cmd_type, cmd_size);
    result = request_result.status;

    if (result >= APIR_FORWARD_BASE_INDEX) {
        /* Use actual response size from Windows service */
        size_t actual_response_size = request_result.actual_response_size;

        temp_reply_buf.fd = open(temp_reply_buf.file_path, O_RDONLY);
        if (temp_reply_buf.fd >= 0) {
            if (actual_response_size > 0 && actual_response_size <= 16 * 1024 * 1024) {
                /* Allocate persistent buffer for reply data */
                static char persistent_reply_buffer[16 * 1024 * 1024];

                ssize_t bytes_read = read(temp_reply_buf.fd, persistent_reply_buffer, actual_response_size);

                if (bytes_read > 0 && bytes_read <= (ssize_t)actual_response_size) {
                    /* Create decoder using proper initialization */
                    *dec = apir_decoder_init(persistent_reply_buffer, (size_t)bytes_read);
                    if (*dec == NULL) {
                        printf("Failed to initialize apir_decoder\n");
                    } else {
                        /* Check backend return code before allowing data access */
                        uint32_t backend_return_code;
                        if (apir_decoder_peek_internal(*dec, sizeof(uint32_t), &backend_return_code, sizeof(uint32_t))) {
                            if (backend_return_code != 0) {
                                printf("[REMOTE_CALL] %s: Backend returned error code %u, aborting\n",
                                       frontend_command_name(actual_cmd_type), backend_return_code);
                                apir_decoder_deinit(*dec);
                                *dec = NULL;
                                _exit(1);
                            } else {
                                /* Skip the return code so decoder is positioned at actual data */
                                apir_decode_uint32_t(*dec, &backend_return_code);
                            }
                        } else {
                            printf("Failed to read backend return code\n");
                            apir_decoder_deinit(*dec);
                            *dec = NULL;
                            result = APIR_FORWARD_HYPERCALL_ERROR;
                        }
                    }
                } else {
                    printf("Failed to read expected %zu bytes from temp reply file (got %zd)\n", actual_response_size, bytes_read);
                    *dec = NULL;
                }
            } else {
                printf("Invalid response size: %zu bytes\n", actual_response_size);
                *dec = NULL;
            }
            close(temp_reply_buf.fd);
        } else {
            printf("Failed to open temp reply file: %s\n", temp_reply_buf.file_path);
            *dec = NULL;
        }
    } else {
        *dec = NULL;
    }

    /* Cleanup temporary files - safe to do now since we copied the data */
    cleanup_temp_file_buffer(&temp_cmd_buf);
    cleanup_temp_file_buffer(&temp_reply_buf);

    return result;
}

static void windows_remote_call_finish(virtgpu* gpu, struct apir_encoder* enc, struct apir_decoder* dec) {
    if (gpu && gpu->backend_data) {
        virtgpu_windows_data* win_data = (virtgpu_windows_data*)gpu->backend_data;

        /* Clean up temporary files */
        if (win_data->temp_request.cmd_file_path[0]) {
            unlink(win_data->temp_request.cmd_file_path);
            win_data->temp_request.cmd_file_path[0] = '\0';
        }
        if (win_data->temp_request.reply_file_path[0]) {
            unlink(win_data->temp_request.reply_file_path);
            win_data->temp_request.reply_file_path[0] = '\0';
        }

        /* Free temporary command buffer */
        if (win_data->temp_request.temp_cmd_data) {
            free(win_data->temp_request.temp_cmd_data);
            win_data->temp_request.temp_cmd_data = NULL;
            win_data->temp_request.temp_cmd_data_size = 0;
        }
    }

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

#if 0
    /* DEBUG: Protect Buffer 1 (89MB) to catch why model weights aren't being written */
    if (size == 89941248 && !debug_first_write_caught) {  // Buffer 1 should get model weights but doesn't
        // Protect this buffer to catch first write with GDB
        if (mprotect(shmem->mmap_ptr, size, PROT_NONE) == 0) {
            debug_protected_buffer = shmem->mmap_ptr;
            debug_protected_size = size;
            printf("[PROTECT_BUFFER_1] Protected Buffer 1 %p size %zu (res_id=%u) - should get model weights!\n",
                   debug_protected_buffer, debug_protected_size, shmem->res_id);
            debug_first_write_caught = true; // Only protect Buffer 1
        } else {
            printf("[PROTECT_BUFFER_1] Could not protect Buffer 1 %p: %s\n", shmem->mmap_ptr, strerror(errno));
        }
    } else if (size > 1024 * 1024) {
        printf("[BUFFER_DEBUG] Buffer allocated %p size %zu (res_id=%u)\n",
               shmem->mmap_ptr, size, shmem->res_id);
    }
#endif

    /* Register buffer with Windows service */
    ret = ggml_winapi_register_buffer(win_data->winapi_handle, &shmem_data->buffer);
    if (ret != GGML_WINAPI_OK) {
        printf("Failed to register buffer %u with Windows service (ret=%d)\n", shmem_data->buffer.buffer_id, ret);
        ggml_winapi_free_shared_buffer(&shmem_data->buffer);
        free(shmem_data);
        return ret;
    }

    return 0;
}

static void windows_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem) {
    (void)gpu; // unused in Windows implementation
    if (shmem && shmem->backend_data) {
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
