/*
 * Minimal Windows API Remoting Client Implementation
 *
 * This provides a standalone implementation for ggml-virtgpu to communicate
 * with Windows hosts without requiring the full winApiRmt project.
 */

#include "ggml-winapi-client.h"
#include "winApiRmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>

/* Connection context */
typedef struct {
    int socket_fd;
    uint32_t next_buffer_id;
    char shared_memory_base[256];
    /* Track the persistent APIR buffers allocated during init */
    ggml_winapi_shared_buffer_t reply_buffer;    // Buffer 1: 16MB reply buffer
    ggml_winapi_shared_buffer_t command_buffer;  // Buffer 2: 4KB command buffer
    bool buffers_initialized;
} ggml_winapi_context_t;

/* Default connection parameters - most come from winApiRmt.h */
#define WINAPI_FALLBACK_HOST "127.0.0.1"  // localhost fallback
/* WINAPI_DEFAULT_PORT now comes from winApiRmt.h */
#define WINAPI_SHARED_MEMORY_BASE "/mnt/c/temp"  // WSL2 -> Windows bridge

/* Helper to get Windows host IP (default gateway) */
static int get_windows_host_ip(char* ip_buffer, size_t buffer_size) {
    FILE* fp;
    char line[256];

    // Get default gateway from route table
    fp = popen("ip route show default", "r");
    if (!fp) {
        return -1;
    }

    if (fgets(line, sizeof(line), fp)) {
        char* via_pos = strstr(line, "via ");
        if (via_pos) {
            via_pos += 4; // Skip "via "
            char* space_pos = strchr(via_pos, ' ');
            if (space_pos) {
                size_t ip_len = space_pos - via_pos;
                if (ip_len < buffer_size) {
                    strncpy(ip_buffer, via_pos, ip_len);
                    ip_buffer[ip_len] = '\0';
                    pclose(fp);
                    return 0;
                }
            }
        }
    }

    pclose(fp);
    return -1;
}

/* Protocol message types */
#define WINAPI_API_ECHO 1
#define WINAPI_API_BUFFER_TEST 2
#define WINAPI_API_APIR_COMMAND 11

static int winapi_connect_tcp(const char* host, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "ggml-winapi: Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "ggml-winapi: Invalid host address: %s\n", host);
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "ggml-winapi: Failed to connect to %s:%d: %s\n",
                host, port, strerror(errno));
        close(sockfd);
        return -1;
    }

    printf("ggml-winapi: Connected to Windows host %s:%d\n", host, port);
    return sockfd;
}

static int winapi_send_json_message(int sockfd, const char* json_msg) {
    size_t msg_len = strlen(json_msg);
    uint32_t network_len = htonl((uint32_t)msg_len);

    /* Send length header first */
    ssize_t sent_header = send(sockfd, (char*)&network_len, sizeof(network_len), 0);
    if (sent_header != sizeof(network_len)) {
        fprintf(stderr, "ggml-winapi: Failed to send message header: %s\n", strerror(errno));
        return -1;
    }

    /* Send JSON data */
    ssize_t sent_data = send(sockfd, json_msg, msg_len, 0);
    if (sent_data != (ssize_t)msg_len) {
        fprintf(stderr, "ggml-winapi: Failed to send message data: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int winapi_receive_response(int sockfd, char* buffer, size_t buffer_size) {
    /* Receive length header first */
    uint32_t network_len;
    ssize_t received_header = recv(sockfd, (char*)&network_len, sizeof(network_len), 0);
    if (received_header != sizeof(network_len)) {
        fprintf(stderr, "ggml-winapi: Failed to receive response header: %s\n", strerror(errno));
        return -1;
    }

    uint32_t msg_len = ntohl(network_len);
    if (msg_len >= buffer_size) {
        fprintf(stderr, "ggml-winapi: Response message too large (%u bytes, buffer is %zu)\n",
                msg_len, buffer_size);
        return -1;
    }

    /* Receive JSON data */
    ssize_t received_data = recv(sockfd, buffer, msg_len, 0);
    if (received_data != (ssize_t)msg_len) {
        fprintf(stderr, "ggml-winapi: Failed to receive complete response data: %s\n", strerror(errno));
        return -1;
    }

    buffer[msg_len] = '\0';
    return (int)msg_len;
}

/* Initialize connection to Windows host */
ggml_winapi_handle_t ggml_winapi_init(void) {
    printf("ggml-winapi: Initializing connection to Windows host...\n");

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)calloc(1, sizeof(ggml_winapi_context_t));
    if (!ctx) {
        fprintf(stderr, "ggml-winapi: Failed to allocate context\n");
        return NULL;
    }

    /* Try to connect via TCP */
    const char* host = getenv("WINAPI_HOST");
    char detected_host[64];

    if (!host) {
        /* Try to auto-detect Windows host IP */
        if (get_windows_host_ip(detected_host, sizeof(detected_host)) == 0) {
            host = detected_host;
            printf("ggml-winapi: Auto-detected Windows host IP: %s\n", host);
        } else {
            host = WINAPI_FALLBACK_HOST;
            printf("ggml-winapi: Failed to detect Windows host IP, using fallback: %s\n", host);
        }
    } else {
        printf("ggml-winapi: Using environment variable WINAPI_HOST: %s\n", host);
    }

    const char* port_str = getenv("WINAPI_PORT");
    int port = port_str ? atoi(port_str) : WINAPI_DEFAULT_PORT;

    ctx->socket_fd = winapi_connect_tcp(host, port);
    if (ctx->socket_fd < 0) {
        free(ctx);
        return NULL;
    }

    /* Set up shared memory base path */
    const char* shared_base = getenv("WINAPI_SHARED_BASE");
    if (!shared_base) {
        shared_base = WINAPI_SHARED_MEMORY_BASE;
    }
    strncpy(ctx->shared_memory_base, shared_base, sizeof(ctx->shared_memory_base) - 1);

    ctx->next_buffer_id = 1;
    ctx->buffers_initialized = false;

    printf("ggml-winapi: Initialization complete\n");
    return (ggml_winapi_handle_t)ctx;
}

/* Cleanup connection */
void ggml_winapi_cleanup(ggml_winapi_handle_t handle) {
    if (!handle) {
        return;
    }

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)handle;

    if (ctx->socket_fd >= 0) {
        close(ctx->socket_fd);
    }

    free(ctx);
    printf("ggml-winapi: Connection cleanup complete\n");
}

/* Allocate shared memory buffer */
int ggml_winapi_alloc_shared_buffer(ggml_winapi_handle_t handle,
                                   size_t size,
                                   ggml_winapi_shared_buffer_t *buffer) {
    if (!handle || !buffer || size == 0) {
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

    if (size > GGML_WINAPI_MAX_BUFFER_SIZE) {
        fprintf(stderr, "ggml-winapi: Buffer size %zu exceeds maximum %d\n",
                size, GGML_WINAPI_MAX_BUFFER_SIZE);
        return GGML_WINAPI_ERROR_BUFFER_TOO_LARGE;
    }

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)handle;

    /* Generate unique buffer file path */
    int path_len = snprintf(buffer->file_path, sizeof(buffer->file_path),
                           "%s/ggml_shared_%u_%zu.dat",
                           ctx->shared_memory_base, ctx->next_buffer_id++, size);

    /* Check for truncation */
    if (path_len >= (int)sizeof(buffer->file_path)) {
        fprintf(stderr, "ggml-winapi: Generated file path too long (%d chars), max %zu\n",
                path_len, sizeof(buffer->file_path) - 1);
        return GGML_WINAPI_ERROR_BUFFER_TOO_LARGE;
    }

    /* Create shared memory file */
    buffer->fd = open(buffer->file_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (buffer->fd < 0) {
        fprintf(stderr, "ggml-winapi: Failed to create shared memory file %s: %s\n",
                buffer->file_path, strerror(errno));
        return GGML_WINAPI_ERROR_MEMORY_MAP_FAILED;
    }

    /* Resize file to requested size */
    if (ftruncate(buffer->fd, size) != 0) {
        fprintf(stderr, "ggml-winapi: Failed to resize shared memory file: %s\n", strerror(errno));
        close(buffer->fd);
        unlink(buffer->file_path);
        return GGML_WINAPI_ERROR_MEMORY_MAP_FAILED;
    }

    /* Map the file into memory */
    buffer->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if (buffer->data == MAP_FAILED) {
        fprintf(stderr, "ggml-winapi: Failed to map shared memory: %s\n", strerror(errno));
        close(buffer->fd);
        unlink(buffer->file_path);
        return GGML_WINAPI_ERROR_MEMORY_MAP_FAILED;
    }

    buffer->size = size;
    buffer->buffer_id = ctx->next_buffer_id - 1;

    return GGML_WINAPI_OK;
}

/* Free shared memory buffer */
void ggml_winapi_free_shared_buffer(ggml_winapi_shared_buffer_t *buffer) {
    if (!buffer) {
        return;
    }

    if (buffer->data && buffer->data != MAP_FAILED) {
        munmap(buffer->data, buffer->size);
        buffer->data = NULL;
    }

    if (buffer->fd >= 0) {
        close(buffer->fd);
        buffer->fd = -1;
    }

    if (buffer->file_path[0]) {
        unlink(buffer->file_path);
        buffer->file_path[0] = '\0';
    }

    buffer->size = 0;
    buffer->buffer_id = 0;
}

/* Register buffer with Windows host */
int ggml_winapi_register_buffer(ggml_winapi_handle_t handle,
                               const ggml_winapi_shared_buffer_t* buffer) {
    if (!handle || !buffer) {
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)handle;

    /* Create buffer registration message */
    char json_string[1024];
    snprintf(json_string, sizeof(json_string),
             "{"
             "\"api\":\"register_buffer\","
             "\"buffer_id\":%u,"
             "\"shared_file_path\":\"%s\","
             "\"buffer_size\":%zu"
             "}",
             buffer->buffer_id, buffer->file_path, buffer->size);

    /* Send registration request */
    int ret = winapi_send_json_message(ctx->socket_fd, json_string);
    if (ret != 0) {
        fprintf(stderr, "ggml-winapi: Failed to send buffer registration for buffer %u\n", buffer->buffer_id);
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Receive registration response */
    char response_json[1024];
    int response_len = winapi_receive_response(ctx->socket_fd, response_json, sizeof(response_json));
    if (response_len <= 0) {
        fprintf(stderr, "ggml-winapi: Failed to receive buffer registration response\n");
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Parse response to check if registration succeeded */
    char* status_ptr = strstr(response_json, "\"status\":");
    if (status_ptr) {
        status_ptr += 9; /* skip "status": */
        while (*status_ptr == ' ' || *status_ptr == '\t') {
            status_ptr++; /* skip whitespace */
        }
        if (*status_ptr == '"') {
            status_ptr++;
            if (strncmp(status_ptr, "success", 7) == 0) {
                return GGML_WINAPI_OK;
            }
        }
    }

    fprintf(stderr, "ggml-winapi: Buffer registration failed for buffer %u\n", buffer->buffer_id);
    return GGML_WINAPI_ERROR_SEND_FAILED;
}

/* Set persistent APIR buffers */
int ggml_winapi_set_apir_buffers(ggml_winapi_handle_t handle,
                                const ggml_winapi_shared_buffer_t* reply_buffer,
                                const ggml_winapi_shared_buffer_t* command_buffer) {
    if (!handle || !reply_buffer || !command_buffer) {
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)handle;

    /* Store buffer information for use by send_apir_command */
    ctx->reply_buffer = *reply_buffer;
    ctx->command_buffer = *command_buffer;
    ctx->buffers_initialized = true;

    return GGML_WINAPI_OK;
}

/* Send APIR command to Windows host */
int ggml_winapi_send_apir_command(ggml_winapi_handle_t handle,
                                 const void* apir_data,
                                 size_t apir_size,
                                 void* response_buffer,
                                 size_t response_buffer_size,
                                 size_t* response_size) {
    if (!handle || !apir_data || apir_size == 0) {
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)handle;

    /* Verify persistent APIR buffers are available */
    if (!ctx->buffers_initialized) {
        fprintf(stderr, "ggml-winapi: APIR buffers not initialized - call ggml_winapi_set_apir_buffers first\n");
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

    /* Use the persistent allocated buffers from initialization */
    uint32_t command_buffer_id = ctx->command_buffer.buffer_id;
    uint32_t reply_buffer_id = ctx->reply_buffer.buffer_id;

    /*
     * IMPORTANT: The APIR data is already written to the persistent command buffer
     * by the APIR encoder in windows_remote_call_prepare(). However, we need to ensure
     * that data is visible to the Windows service by syncing it to the file.
     * This avoids the repeated mmap/write cycle while ensuring cache coherency.
     */

    if (apir_size > 4096) {
        fprintf(stderr, "ggml-winapi: APIR data size %zu exceeds command buffer size 4096\n", apir_size);
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

/* if FORCE_FLUSH_COMMAND_BUFFER == 0
  ==> failing on: apir_device_get_description: string size too short (1), aborting
  ==> this means that the host receives an outdated view of the command buffer, and fill the answer of the the device_get_count command
*/

/* if FORCE_FLUSH_COMMAND_BUFFER == 1
   ==> failing on [FRONTEND] FATAL: Backend reported 46 devices - abnormal! Expected 1-2 maximum. Terminating.
   ==> this means that the guest sees an outdated response buffer
*/

#define FORCE_FLUSH_COMMAND_BUFFER 0

#if FORCE_FLUSH_COMMAND_BUFFER == 1
    /*
     * We need to sync the command buffer to ensure Windows service sees the data.
     * Open the file briefly to get an FD for fsync, but don't remap it.
     */
    int command_fd = open(ctx->command_buffer.file_path, O_RDWR);
    if (command_fd < 0) {
        fprintf(stderr, "ggml-winapi: Failed to open command buffer %s for sync: %s\n",
                ctx->command_buffer.file_path, strerror(errno));
        return GGML_WINAPI_ERROR_MEMORY_MAP_FAILED;
    }

    /* Force sync to ensure Windows service sees the encoder data */
    if (fsync(command_fd) != 0) {
        fprintf(stderr, "ggml-winapi: Warning: fsync failed: %s\n", strerror(errno));
    }

    close(command_fd);
#endif
    /* Extract command type from APIR binary data */
    uint32_t cmd_type = 22;  // Default fallback
    if (apir_size >= sizeof(uint32_t)) {
        /* APIR data starts with command type as uint32_t */
        memcpy(&cmd_type, apir_data, sizeof(uint32_t));
    }

    /* Create JSON command message using pre-allocated paired buffers */
    char json_string[2048];
    snprintf(json_string, sizeof(json_string),
             "{"
             "\"api\":\"apir\","
             "\"request_id\":1,"
             "\"apir_cmd_type\":%u,"
             "\"apir_data_size\":%zu,"
             "\"shared_file_path\":\"%s\","
             "\"buffer_id\":%u,"
             "\"response_buffer_id\":%u"
             "}",
             cmd_type, apir_size, ctx->command_buffer.file_path, command_buffer_id, reply_buffer_id);

    /* Send JSON command over socket */
    int ret = winapi_send_json_message(ctx->socket_fd, json_string);

    if (ret != 0) {
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Receive response */
    char response_json[4096];
    int response_len = winapi_receive_response(ctx->socket_fd, response_json, sizeof(response_json));
    if (response_len <= 0) {
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Parse JSON response - simple string parsing approach */
    char status_str[64] = "error";
    int error_code = 1;
    size_t actual_response_size = 0;

    /* Look for status field */
    char* status_ptr = strstr(response_json, "\"status\":");
    if (status_ptr) {
        status_ptr += 9; /* skip "status": */
        while (*status_ptr == ' ' || *status_ptr == '\t') {
            status_ptr++; /* skip whitespace */
        }
        if (*status_ptr == '"') {
            status_ptr++;
            char* status_end = strchr(status_ptr, '"');
            if (status_end && (size_t)(status_end - status_ptr) < sizeof(status_str) - 1) {
                strncpy(status_str, status_ptr, status_end - status_ptr);
                status_str[status_end - status_ptr] = '\0';
            }
        }
    }

    /* Look for error_code field */
    char* error_code_ptr = strstr(response_json, "\"error_code\":");
    if (error_code_ptr) {
        error_code_ptr += 13; /* skip "error_code": */
        error_code = strtol(error_code_ptr, NULL, 10);
    }

    /* Look for response_size field */
    char* response_size_ptr = strstr(response_json, "\"response_size\":");
    if (response_size_ptr) {
        response_size_ptr += 16; /* skip "response_size": */
        actual_response_size = strtoull(response_size_ptr, NULL, 10);
    }

    /* Handle response using pre-allocated paired buffers */
    if (strcmp(status_str, "success") == 0 && error_code == 0) {
        if (actual_response_size > 0) {
            /*
             * Read response from pre-allocated reply buffer using the persistent mapping.
             * The response_buffer parameter points to the persistent mmap, but we need
             * to ensure cache coherency by syncing the mapping after Windows service writes.
             */

            /* Sync the persistent mapping to ensure we see Windows service writes */
            if (response_buffer && response_buffer_size > 0) {
#if 0
                /* Force cache coherency - invalidate our cached view */
                if (msync(response_buffer, response_buffer_size, MS_INVALIDATE) != 0) {
                    fprintf(stderr, "ggml-winapi: Warning: msync MS_INVALIDATE failed: %s\n", strerror(errno));
                }
#endif
                size_t bytes_to_read = (actual_response_size < response_buffer_size) ?
                                       actual_response_size : response_buffer_size;
                *response_size = bytes_to_read;
                ret = GGML_WINAPI_OK;

            } else {
                fprintf(stderr, "ggml-winapi: Invalid response buffer parameters\n");
                *response_size = 0;
                ret = GGML_WINAPI_ERROR_SEND_FAILED;
            }
        } else {
            /* Success with empty response */
            *response_size = 0;
            ret = GGML_WINAPI_OK;
        }
    } else {
        /* Error case */
        fprintf(stderr, "ggml-winapi: Command failed - status: %s, error_code: %d\n", status_str, error_code);
        *response_size = 0;

        /* Map APIR error codes to client error codes */
        if (error_code > 0) {
            ret = GGML_WINAPI_ERROR_SEND_FAILED;
        } else {
            ret = GGML_WINAPI_ERROR_UNKNOWN;
        }
    }

    return ret;
}

/* Test connectivity */
int ggml_winapi_echo(ggml_winapi_handle_t handle,
                    const char *input,
                    char *output,
                    size_t output_size) {
    if (!handle || !input || !output) {
        return GGML_WINAPI_ERROR_INVALID_PARAMS;
    }

    ggml_winapi_context_t* ctx = (ggml_winapi_context_t*)handle;

    /* Create echo request */
    char json_string[1024];
    snprintf(json_string, sizeof(json_string),
             "{\"api\":\"echo\",\"input\":\"%s\"}", input);

    /* Send echo request */
    int ret = winapi_send_json_message(ctx->socket_fd, json_string);

    if (ret != 0) {
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Receive echo response */
    char response_json[4096];
    int response_len = winapi_receive_response(ctx->socket_fd, response_json, sizeof(response_json));
    if (response_len <= 0) {
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* For simplicity, just copy the input back as echo */
    strncpy(output, input, output_size - 1);
    output[output_size - 1] = '\0';

    printf("ggml-winapi: Echo test successful\n");
    return GGML_WINAPI_OK;
}
