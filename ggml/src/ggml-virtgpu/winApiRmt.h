/*
 * Windows API Remoting Backend Header
 *
 * This header provides the Windows WinAPI backend interface for VirtGPU operations.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "./virtgpu-interface.h"
#include "ggml-winapi-client.h"

/* Windows-specific constants */
#define WINAPI_DEFAULT_HOST       "127.0.0.1"
#define WINAPI_DEFAULT_PORT       4660
#define WINAPI_SHARED_MEMORY_BASE "/mnt/c/temp"

/* Windows backend data structures */
typedef struct {
    ggml_winapi_handle_t winapi_handle;  // Windows API Remoting client handle
    bool connection_established;         // Whether connection is active
    char shared_memory_base[256];        // Base path for shared memory files
} winapi_backend_data;

typedef struct {
    ggml_winapi_shared_buffer_t buffer;  // Windows shared buffer implementation
    uint32_t buffer_id;                  // Buffer ID for protocol
    size_t allocated_size;               // Allocated size
} winapi_shmem_data;

/* Public interface function */
const virtgpu_backend_ops* virtgpu_backend_windows_winapi_get_ops(void);

/* Windows-specific utility functions */
/*
 * These functions are implemented in winApiRmt.c:
 *
 * - winapi_connect() - Establish TCP connection to Windows host
 * - winapi_disconnect() - Close connection
 * - winapi_send_json() - Send JSON protocol message
 * - winapi_receive_response() - Receive JSON response
 * - winapi_alloc_shared_buffer() - Create shared memory file
 * - winapi_free_shared_buffer() - Destroy shared memory file
 * - winapi_encode_apir_command() - Encode APIR data for transmission
 */

/* JSON Protocol message types */
#define WINAPI_API_ECHO           1
#define WINAPI_API_BUFFER_TEST    2
#define WINAPI_API_APIR_COMMAND   11

/* Return codes */
#define WINAPI_OK                 0
#define WINAPI_ERROR_CONNECTION   1
#define WINAPI_ERROR_PROTOCOL     2
#define WINAPI_ERROR_MEMORY       3

#ifdef __cplusplus
}
#endif
