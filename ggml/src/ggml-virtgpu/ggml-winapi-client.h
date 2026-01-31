/*
 * Minimal Windows API Remoting Client for ggml-virtgpu
 *
 * This is a standalone implementation that provides only the essential
 * functions needed by ggml-virtgpu to communicate with Windows hosts.
 *
 * Eliminates the need for the full winApiRmt project dependency.
 */

#ifndef GGML_WINAPI_CLIENT_H
#define GGML_WINAPI_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Connection handle - opaque pointer */
typedef void* ggml_winapi_handle_t;

/* Shared buffer structure */
typedef struct {
    void *data;              // Mapped memory pointer
    size_t size;             // Buffer size in bytes
    char file_path[256];     // Path to backing file
    int fd;                  // File descriptor (Linux)
    uint32_t buffer_id;      // Unique buffer identifier
} ggml_winapi_shared_buffer_t;

/* Error codes */
typedef enum {
    GGML_WINAPI_OK = 0,
    GGML_WINAPI_ERROR_INVALID_PARAMS = -1,
    GGML_WINAPI_ERROR_CONNECTION_FAILED = -2,
    GGML_WINAPI_ERROR_MEMORY_MAP_FAILED = -3,
    GGML_WINAPI_ERROR_BUFFER_TOO_LARGE = -4,
    GGML_WINAPI_ERROR_SEND_FAILED = -5,
    GGML_WINAPI_ERROR_UNKNOWN = -99
} ggml_winapi_error_t;

/* Protocol constants */
#define GGML_WINAPI_MAGIC 0x41504952            // "APIR"
#define GGML_WINAPI_MAX_BUFFER_SIZE (256 * 1024 * 1024)  // 256MB

/*
 * Core API Functions
 * These provide the minimal interface needed for ggml-virtgpu
 */

/* Initialize connection to Windows host */
ggml_winapi_handle_t ggml_winapi_init(void);

/* Cleanup connection */
void ggml_winapi_cleanup(ggml_winapi_handle_t handle);

/* Allocate shared memory buffer */
int ggml_winapi_alloc_shared_buffer(ggml_winapi_handle_t handle,
                                   size_t size,
                                   ggml_winapi_shared_buffer_t *buffer);

/* Free shared memory buffer */
void ggml_winapi_free_shared_buffer(ggml_winapi_shared_buffer_t *buffer);

/* Register buffer with Windows host */
int ggml_winapi_register_buffer(ggml_winapi_handle_t handle,
                               const ggml_winapi_shared_buffer_t* buffer);

/* Set persistent APIR buffers (called during backend initialization) */
int ggml_winapi_set_apir_buffers(ggml_winapi_handle_t handle,
                                const ggml_winapi_shared_buffer_t* reply_buffer,
                                const ggml_winapi_shared_buffer_t* command_buffer);

/* Send APIR command to Windows host */
int ggml_winapi_send_apir_command(ggml_winapi_handle_t handle,
                                 const void* apir_data,
                                 size_t apir_size,
                                 void* response_buffer,
                                 size_t response_buffer_size,
                                 size_t* response_size);

/* Send APIR command using temporary files */
int ggml_winapi_send_temp_file_request(ggml_winapi_handle_t handle,
                                      const char* cmd_file_path,
                                      const char* reply_file_path,
                                      size_t cmd_data_size,
                                      size_t* actual_response_size);

/* Test connectivity (optional - for debugging) */
int ggml_winapi_echo(ggml_winapi_handle_t handle,
                    const char *input,
                    char *output,
                    size_t output_size);

/*
 * Compatibility aliases for existing code
 * These map to the new ggml_winapi_* functions
 */
typedef ggml_winapi_handle_t winapi_handle_t;
typedef ggml_winapi_shared_buffer_t winapi_shared_buffer_t;

#define winapi_init()                    ggml_winapi_init()
#define winapi_cleanup(h)                ggml_winapi_cleanup(h)
#define winapi_alloc_shared_buffer(h,s,b) ggml_winapi_alloc_shared_buffer(h,s,b)
#define winapi_free_shared_buffer(b)     ggml_winapi_free_shared_buffer(b)
#define winapi_send_apir_command(h,d,s,r,rs,rsz) ggml_winapi_send_apir_command(h,d,s,r,rs,rsz)
#define winapi_echo(h,i,o,s)             ggml_winapi_echo(h,i,o,s)

#ifdef __cplusplus
}
#endif

#endif /* GGML_WINAPI_CLIENT_H */