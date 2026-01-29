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

    /* Create shared buffer for APIR data */
    ggml_winapi_shared_buffer_t apir_buffer;
    int ret = ggml_winapi_alloc_shared_buffer(handle, apir_size, &apir_buffer);
    if (ret != GGML_WINAPI_OK) {
        return ret;
    }

    /* Copy APIR data into shared buffer */
    memcpy(apir_buffer.data, apir_data, apir_size);

    /* Force sync memory-mapped data to disk before Windows service reads it */
    if (msync(apir_buffer.data, apir_size, MS_SYNC) != 0) {
        fprintf(stderr, "ggml-winapi: Warning: msync failed: %s\n", strerror(errno));
    }

    /* Also sync the file descriptor */
    if (fsync(apir_buffer.fd) != 0) {
        fprintf(stderr, "ggml-winapi: Warning: fsync failed: %s\n", strerror(errno));
    }

    /* Extract command type from APIR binary data */
    uint32_t cmd_type = 22;  // Default fallback
    if (apir_size >= sizeof(uint32_t)) {
        /* APIR data starts with command type as uint32_t */
        memcpy(&cmd_type, apir_data, sizeof(uint32_t));
    }

    /* Create JSON command message */
    char json_string[2048];
    snprintf(json_string, sizeof(json_string),
             "{"
             "\"api\":\"apir\","
             "\"request_id\":1,"
             "\"apir_cmd_type\":%u,"
             "\"apir_data_size\":%zu,"
             "\"shared_file_path\":\"%s\","
             "\"buffer_id\":%u"
             "}",
             cmd_type, apir_size, apir_buffer.file_path, apir_buffer.buffer_id);

    /* Send JSON command over socket */
    ret = winapi_send_json_message(ctx->socket_fd, json_string);

    if (ret != 0) {
        ggml_winapi_free_shared_buffer(&apir_buffer);
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Receive response */
    char response_json[4096];
    int response_len = winapi_receive_response(ctx->socket_fd, response_json, sizeof(response_json));
    if (response_len <= 0) {
        ggml_winapi_free_shared_buffer(&apir_buffer);
        return GGML_WINAPI_ERROR_SEND_FAILED;
    }

    /* Parse JSON response - simple string parsing approach */
    char status_str[64] = "error";
    int error_code = 1;
    size_t actual_response_size = 0;
    char response_file_path[512] = "";

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

    /* Look for response_file_path field */
    char* file_path_ptr = strstr(response_json, "\"response_file_path\":");
    if (file_path_ptr) {
        file_path_ptr += 21; /* skip "response_file_path": */
        while (*file_path_ptr == ' ' || *file_path_ptr == '\t') {
            file_path_ptr++; /* skip whitespace */
        }
        if (*file_path_ptr == '"') {
            file_path_ptr++;
            char* file_path_end = strchr(file_path_ptr, '"');
            if (file_path_end && (size_t)(file_path_end - file_path_ptr) < sizeof(response_file_path) - 1) {
                strncpy(response_file_path, file_path_ptr, file_path_end - file_path_ptr);
                response_file_path[file_path_end - file_path_ptr] = '\0';
            }
        }
    }

    /* Handle successful response with binary data */
    if (strcmp(status_str, "success") == 0 && error_code == 0) {
        if (strlen(response_file_path) > 0) {
            /* Read binary response data from file */
            int response_fd = open(response_file_path, O_RDONLY);
            if (response_fd >= 0) {
                size_t bytes_to_read = (actual_response_size < response_buffer_size) ?
                                       actual_response_size : response_buffer_size;
                ssize_t bytes_read = read(response_fd, response_buffer, bytes_to_read);
                close(response_fd);

                if (bytes_read > 0) {
                    *response_size = bytes_read;

                    /* Clean up response file */
                    unlink(response_file_path);
                } else {
                    fprintf(stderr, "ggml-winapi: Failed to read response data from %s\n", response_file_path);
                    *response_size = 0;
                    ret = GGML_WINAPI_ERROR_SEND_FAILED;
                }
            } else {
                fprintf(stderr, "ggml-winapi: Failed to open response file: %s\n", response_file_path);
                perror("ggml-winapi: open() error");
                *response_size = 0;
                ret = GGML_WINAPI_ERROR_SEND_FAILED;
            }
        } else {
            /* Success but no response data (command completed with empty result) */
            *response_size = 0;
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
    ggml_winapi_free_shared_buffer(&apir_buffer);
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