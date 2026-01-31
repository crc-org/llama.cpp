#ifdef GGML_VIRTGPU_USE_WINDOWS
#include "virtgpu-interface.h"
#include "ggml-winapi-client.h"  // For ggml_winapi_shared_buffer_t
#include <threads.h>  // For mtx_t, mtx_lock, mtx_unlock
#else
#include "virtgpu.h"
#endif

#include "ggml-remoting.h"
#include "backend/shared/apir_backend.h"
#include "backend/shared/apir_cs_ggml.h"
#include "backend/shared/api_remoting.h"
#include "backend/shared/apir_cs.h"
#include "apir-minimal.h"

#include "../ggml-backend-impl.h"

#include <sys/mman.h>  // For madvise and MADV_DONTNEED
#include <fcntl.h>     // For open()
#include <unistd.h>    // For fsync(), close()
#include <stdatomic.h> // For atomic operations
#include <errno.h>     // For errno
#include <string.h>    // For strerror()
#include <sys/socket.h> // For send(), recv()
#include <netinet/in.h> // For ntohl(), htonl()
#include <stdlib.h>    // For strtoul()

/* Temporary file management for per-request communication */
typedef struct {
    char file_path[256];
    int fd;
    void* mmap_ptr;
    size_t size;
} temp_file_buffer_t;

/* Result of temporary file request */
typedef struct {
    ApirForwardReturnCode status;
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
        int32_t forward_flag = (int32_t) (apir_command_type__);                                            \
        const char * method_name = frontend_command_name(forward_flag);                                    \
        printf("[FRONTEND] Calling method: %s (cmd_type=%d)\n", method_name, forward_flag);             \
        (encoder_name)       = remote_call_prepare((gpu_dev_name), APIR_COMMAND_TYPE_FORWARD, forward_flag); \
        if (!(encoder_name)) {                                                                             \
            printf("FATAL: %s: failed to prepare the remote call encoder\n", __func__);                 \
            fflush(stdout);                                                                              \
            exit(1);                                                                                     \
        }                                                                                                  \
    } while (0)

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
        (ApirForwardReturnCode)(APIR_FORWARD_BASE_INDEX + 0),
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

/* Execute remote call using temporary files */
static inline ApirForwardReturnCode execute_temp_file_remote_call(virtgpu* gpu, apir_encoder* encoder, apir_decoder** decoder) {
    temp_file_buffer_t temp_cmd_buf;
    temp_file_buffer_t temp_reply_buf;
    ApirForwardReturnCode result;
    uint32_t actual_cmd_type = 0;  /* Declare early for use throughout function */

    /* Initialize structures */
    memset(&temp_cmd_buf, 0, sizeof(temp_cmd_buf));
    memset(&temp_reply_buf, 0, sizeof(temp_reply_buf));
    temp_cmd_buf.fd = -1;
    temp_reply_buf.fd = -1;

    size_t cmd_size = encoder->cur - encoder->start;
    size_t reply_size = 16 * 1024 * 1024; /* 16MB reply buffer */

    /* Create and populate command file */
    if (create_temp_file_buffer(&temp_cmd_buf, "cmd", cmd_size) != 0) {
        printf("Failed to create temp command file\n");
        *decoder = NULL;
        return APIR_FORWARD_HYPERCALL_ERROR;
    }

    memcpy(temp_cmd_buf.mmap_ptr, encoder->start, cmd_size);

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
        *decoder = NULL;
        return APIR_FORWARD_HYPERCALL_ERROR;
    }

    munmap(temp_reply_buf.mmap_ptr, temp_reply_buf.size);
    close(temp_reply_buf.fd);
    temp_reply_buf.mmap_ptr = NULL;
    temp_reply_buf.fd = -1;

    /* Extract function_id from APIR structure for JSON transmission */
    if (cmd_size >= 8) {
        const uint32_t* data_ptr = (const uint32_t*)encoder->start;
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
                    *decoder = apir_decoder_init(persistent_reply_buffer, (size_t)bytes_read);
                    if (*decoder == NULL) {
                        printf("Failed to initialize apir_decoder\n");
                    } else {
                        /* Check backend return code before allowing data access */
                        uint32_t backend_return_code;
                        if (apir_decoder_peek_internal(*decoder, sizeof(uint32_t), &backend_return_code, sizeof(uint32_t))) {
                            if (backend_return_code != 0) {
                                printf("[REMOTE_CALL] Backend returned error code %u, aborting\n", backend_return_code);
                                apir_decoder_deinit(*decoder);
                                *decoder = NULL;
                            } else {
                                /* Skip the return code so decoder is positioned at actual data */
                                apir_decode_uint32_t(*decoder, &backend_return_code);
                            }
                        } else {
                            printf("Failed to read backend return code\n");
                            apir_decoder_deinit(*decoder);
                            *decoder = NULL;
                        }
                    }
                } else {
                    printf("Failed to read expected %zu bytes from temp reply file (got %zd)\n", actual_response_size, bytes_read);
                    *decoder = NULL;
                }
            } else {
                printf("Invalid response size: %zu bytes\n", actual_response_size);
                *decoder = NULL;
            }
            close(temp_reply_buf.fd);
        } else {
            printf("Failed to open temp reply file: %s\n", temp_reply_buf.file_path);
            *decoder = NULL;
        }
    } else {
        *decoder = NULL;
    }

    /* Cleanup temporary files - safe to do now since we copied the data */
    cleanup_temp_file_buffer(&temp_cmd_buf);
    cleanup_temp_file_buffer(&temp_reply_buf);

    return result;
}

#define REMOTE_CALL(gpu_dev_name, encoder_name, decoder_name, ret_name)                                           \
    do {                                                                                                          \
        (ret_name) = execute_temp_file_remote_call((gpu_dev_name), (encoder_name), &(decoder_name));            \
        if (!(decoder_name)) {                                                                                    \
            printf("FATAL: %s: failed to kick the remote call\n", __func__);                                   \
            fflush(stdout);                                                                                      \
            exit(1);                                                                                             \
        }                                                                                                         \
        if ((ret_name) < APIR_FORWARD_BASE_INDEX) {                                                               \
            printf("FATAL: %s: failed to forward the API call: %s: code %d\n", __func__,                       \
                   apir_forward_error((ret_name)), (ret_name));                                                   \
            fflush(stdout);                                                                                        \
            exit(1);                                                                                               \
        }                                                                                                         \
        (ret_name) = (ApirForwardReturnCode) ((ret_name) - APIR_FORWARD_BASE_INDEX);                             \
    } while (0)
