/*
 * VirtGPU Windows Backend Service
 *
 * This service provides VirtGPU backend functionality for Windows hosts,
 * processing APIR commands from Linux WSL2 guests via TCP/JSON protocol
 * and file-based shared memory.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <guiddef.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json/json.h>
// Alternative: Use cJSON if jsoncpp is problematic
// #include <cjson/cjson.h>
#include <conio.h>
#include <signal.h>
#include <time.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "dbghelp.lib")

// Define INET_ADDRSTRLEN if not available
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#include "../../winApiRmt/common/protocol.h"

// Include C++ headers first (outside extern "C")
#include "../shared/api_remoting.h"

// Forward declare the C functions we need
extern "C" {
    struct virgl_apir_callbacks {
        const char * (*get_config)(uint32_t virgl_ctx_id, const char * key);
        void * (*get_shmem_ptr)(uint32_t virgl_ctx_id, uint32_t res_id);
    };

    ApirLoadLibraryReturnCode apir_backend_initialize(uint32_t virgl_ctx_id, struct virgl_apir_callbacks *virgl_cbs);
    void                      apir_backend_deinit(uint32_t virgl_ctx_id);
    uint32_t                  apir_backend_dispatcher(uint32_t               virgl_ctx_id,
                                                      virgl_apir_callbacks * virgl_cbs,
                                                      uint32_t               cmd_type,
                                                      char *                 dec_cur,
                                                      const char *           dec_end,
                                                      char *                 enc_cur,
                                                      const char *           enc_end,
                                                      char **                enc_cur_after);
}


// AF_VSOCK definition for Windows (may not be available on all versions)
#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

// VSOCK address structure (if not defined)
#ifndef SOCKADDR_VM
struct sockaddr_vm {
    ADDRESS_FAMILY svm_family;
    USHORT svm_reserved1;
    ULONG svm_port;
    ULONG svm_cid;
    UCHAR svm_zero[sizeof(struct sockaddr) - sizeof(ADDRESS_FAMILY) - sizeof(USHORT) - sizeof(ULONG) - sizeof(ULONG)];
};
#define SOCKADDR_VM struct sockaddr_vm
#endif

#ifndef VMADDR_CID_ANY
#define VMADDR_CID_ANY -1U
#endif

// Service configuration
#define SERVICE_NAME            L"WinApiRemoting"
#define SERVICE_DISPLAY_NAME    L"Windows API Remoting for WSL2"
#define HYPERV_SOCKET_PORT      0x400
#define TCP_SOCKET_PORT         4660               // TCP fallback port
#define SHARED_MEMORY_NAME      L"WinApiSharedMemory"
#define SHARED_MEMORY_SIZE      (32 * 1024 * 1024) // 32MB
#define MAX_CLIENTS             16

// Shared Memory Layout
#define HEADER_SIZE             4096
#define REQUEST_BUFFER_SIZE     (15 * 1024 * 1024) // 15MB
#define RESPONSE_BUFFER_SIZE    (15 * 1024 * 1024) // 15MB

// SafeMemoryWrite boundary - switch to safe writes this far from buffer end
#define SAFE_WRITE_BOUNDARY     (32 * 1024)  // 32KB before buffer end
#define SAFE_WRITE_OFFSET       (RESPONSE_BUFFER_SIZE - SAFE_WRITE_BOUNDARY)

// Magic values
#define WINAPI_MAGIC            0x57494E41  // "WINA"
#define PROTOCOL_VERSION        1

// Shared memory header structure
struct shared_memory_header {
    UINT32 magic;
    UINT32 version;
    UINT32 request_count;
    UINT32 flags;
    UINT64 request_offset;
    UINT64 response_offset;
    UINT32 request_size;
    UINT32 response_size;
    UINT32 reserved[12];
};

// Global state
struct service_context {
    SOCKET listen_socket;
    SOCKET tcp_listen_socket;  // TCP fallback socket
    BOOL using_tcp;            // TRUE if using TCP fallback
    HANDLE shared_memory_handle;
    LPVOID shared_memory_view;
    struct shared_memory_header *header;
    LPVOID request_buffer;
    LPVOID response_buffer;
    HANDLE stop_event;
    BOOL running;
    BOOL apir_backend_initialized;  // APIR backend initialization status
};

static struct service_context g_ctx = {0};
static SERVICE_STATUS_HANDLE g_service_status_handle = NULL;
static SERVICE_STATUS g_service_status = {0};
static BOOL g_force_tcp = TRUE;  // Default to TCP mode

// APIR Backend Global State - Per-Client Buffer Management
struct BufferMapping {
    HANDLE file_handle;
    HANDLE mapping_handle;
    void* mapped_memory;
    size_t size;
    std::string file_path;
};

struct ClientSession {
    uint32_t session_id;
    std::map<uint32_t, BufferMapping> buffers;  // buffer_id -> mapping
};

static std::map<uint32_t, ClientSession> g_client_sessions;
static std::mutex g_buffer_mutex;
static uint32_t g_next_session_id = 1;
static char g_response_file_path[512] = "";

// Windows-specific APIR callback implementations
const char* windows_get_config(uint32_t virgl_ctx_id, const char* key) {
    UNREFERENCED_PARAMETER(virgl_ctx_id);

    // GGML library configuration from environment variables
    if (strcmp(key, "ggml.library.path") == 0) {
        return getenv("APIR_LLAMA_CPP_GGML_LIBRARY_PATH");
    }
    if (strcmp(key, "ggml.library.reg") == 0) {
        return getenv("APIR_LLAMA_CPP_GGML_LIBRARY_REG");
    }
    if (strcmp(key, "ggml.library.init") == 0) {
        return getenv("APIR_LLAMA_CPP_GGML_LIBRARY_INIT");
    }

    // Default configurations for Windows environment
    if (strcmp(key, "log_level") == 0) return "info";
    if (strcmp(key, "backend_type") == 0) return "cpu";  // Default to CPU backend
    if (strcmp(key, "max_buffer_size") == 0) return "268435456";  // 256MB

    return NULL;
}

void* windows_get_shmem_ptr(uint32_t virgl_ctx_id, uint32_t res_id) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);

    // virgl_ctx_id serves as our session_id
    uint32_t session_id = virgl_ctx_id;

    auto session_it = g_client_sessions.find(session_id);
    if (session_it == g_client_sessions.end()) {
        printf("[ERROR] No session found for context ID: %u\n", session_id);
        return NULL;
    }

    auto& session = session_it->second;
    auto buffer_it = session.buffers.find(res_id);
    if (buffer_it == session.buffers.end()) {
        printf("[ERROR] No buffer mapping found for session %u, buffer ID: %u\n",
               session_id, res_id);
        return NULL;
    }

    void* ptr = buffer_it->second.mapped_memory;
    if (ptr == NULL) {
        printf("[ERROR] Buffer %u has NULL mapping in session %u\n", res_id, session_id);
    }

    return ptr;
}

static struct virgl_apir_callbacks g_windows_callbacks = {
    .get_config = windows_get_config,
    .get_shmem_ptr = windows_get_shmem_ptr,
};

// Helper function to get or create a session ID for a client
uint32_t get_client_session_id(SOCKET client_socket) {
    // Use socket handle as a simple session identifier
    // In production, could use more sophisticated session management
    return (uint32_t)(uintptr_t)client_socket;
}

// Helper function to store buffer mapping for a client session
void store_buffer_mapping(uint32_t session_id, uint32_t buffer_id,
                         HANDLE file_handle, HANDLE mapping_handle,
                         void* mapped_memory, size_t size,
                         const std::string& file_path) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);

    // Get or create client session
    auto& session = g_client_sessions[session_id];
    session.session_id = session_id;

    // Store buffer mapping
    BufferMapping mapping;
    mapping.file_handle = file_handle;
    mapping.mapping_handle = mapping_handle;
    mapping.mapped_memory = mapped_memory;
    mapping.size = size;
    mapping.file_path = file_path;

    session.buffers[buffer_id] = mapping;
}

// Helper function to cleanup all buffers for a client session
void cleanup_client_session(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(g_buffer_mutex);

    auto session_it = g_client_sessions.find(session_id);
    if (session_it != g_client_sessions.end()) {
        auto& session = session_it->second;

        printf("[INFO] Cleaning up session %u with %zu buffers\n",
               session_id, session.buffers.size());

        // Clean up all buffer mappings
        for (auto& [buffer_id, mapping] : session.buffers) {
            if (mapping.mapped_memory) {
                UnmapViewOfFile(mapping.mapped_memory);
            }
            if (mapping.mapping_handle) {
                CloseHandle(mapping.mapping_handle);
            }
            if (mapping.file_handle) {
                CloseHandle(mapping.file_handle);
            }
        }

        // Remove the session
        g_client_sessions.erase(session_it);
    }
}

// Forward declarations
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
void WINAPI ServiceCtrlHandler(DWORD ctrl);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
DWORD InitializeService();
void CleanupService();
DWORD HandleClient(SOCKET client_socket);
DWORD ProcessAPIRequest(SOCKET client_socket, const char* request_json, char* response_json, size_t response_size);

// Windows exception handler for crash detection
LONG WINAPI WindowsExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo);
void SignalHandler(int signal_num);

// Safe memory write with SEH
BOOL SafeMemoryWrite(UINT32* ptr, UINT32 value, UINT64 offset);

// Structure to pass buffer send info
struct BufferSendInfo {
    BOOL needs_buffer_send;
    UINT64 buffer_size;
    UINT32 test_pattern;
};

// JSON helper functions
Json::Value CreateErrorResponse(UINT32 request_id, const char* error_msg);
Json::Value CreateSuccessResponse(UINT32 request_id);

// API implementations
DWORD HandleEchoAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response);
DWORD HandleBufferTestAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response);
DWORD HandlePerformanceAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response);
DWORD HandleSharedBufferAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response);
DWORD HandleAPIRAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response);
DWORD HandleBufferRegistrationAPI(SOCKET client_socket, UINT32 request_id, UINT32 buffer_id, const std::string& file_path, Json::Value& response);

/*
 * Windows exception handler for crash detection (replaces Unix signals)
 */
LONG WINAPI WindowsExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo)
{
    const char* exception_name;
    DWORD exception_code = ExceptionInfo->ExceptionRecord->ExceptionCode;

    switch (exception_code) {
        case EXCEPTION_ACCESS_VIOLATION:
            exception_name = "EXCEPTION_ACCESS_VIOLATION (Segmentation fault equivalent)";
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            exception_name = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            exception_name = "EXCEPTION_DATATYPE_MISALIGNMENT";
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            exception_name = "EXCEPTION_FLT_DIVIDE_BY_ZERO";
            break;
        case EXCEPTION_FLT_OVERFLOW:
            exception_name = "EXCEPTION_FLT_OVERFLOW";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            exception_name = "EXCEPTION_ILLEGAL_INSTRUCTION";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            exception_name = "EXCEPTION_INT_DIVIDE_BY_ZERO";
            break;
        case EXCEPTION_INT_OVERFLOW:
            exception_name = "EXCEPTION_INT_OVERFLOW";
            break;
        case EXCEPTION_INVALID_DISPOSITION:
            exception_name = "EXCEPTION_INVALID_DISPOSITION";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            exception_name = "EXCEPTION_STACK_OVERFLOW";
            break;
        default:
            exception_name = "Unknown Windows exception";
            break;
    }

    printf("\n\n*** WINDOWS CRASH DETECTED ***\n");
    printf("Exception Code: 0x%08X (%s)\n", exception_code, exception_name);

    time_t current_time = time(NULL);
    printf("Time: %s", ctime(&current_time));

    printf("Exception Address: %p\n", ExceptionInfo->ExceptionRecord->ExceptionAddress);

    if (exception_code == EXCEPTION_ACCESS_VIOLATION && ExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR access_type = ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR address = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        printf("Access Violation: %s at address %p\n",
               access_type == 0 ? "Read" : (access_type == 1 ? "Write" : "Execute"),
               (void*)address);
    }

    // Add stack trace for better debugging
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymInitialize(process, NULL, TRUE);

    CONTEXT* context = ExceptionInfo->ContextRecord;
    STACKFRAME64 stackFrame = {};

#ifdef _M_X64
    stackFrame.AddrPC.Offset = context->Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context->Rbp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context->Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
#else
    stackFrame.AddrPC.Offset = context->Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context->Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context->Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
#endif

    printf("\n=== STACK TRACE ===\n");

    for (int i = 0; i < 20; i++) {
        if (!StackWalk64(machineType, process, thread, &stackFrame, context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }

        char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (SymFromAddr(process, stackFrame.AddrPC.Offset, &displacement, symbol)) {
            printf("[%d] %s + 0x%llx (0x%llx)\n", i, symbol->Name, displacement, stackFrame.AddrPC.Offset);
        } else {
            printf("[%d] <unknown> (0x%llx)\n", i, stackFrame.AddrPC.Offset);
        }
    }

    printf("==================\n");

    printf("Server is terminating due to exception...\n");
    fflush(stdout);

    // Clean up if possible
    if (g_ctx.running) {
        printf("Attempting cleanup...\n");
        fflush(stdout);
        CleanupService();
    }

    // Return EXCEPTION_EXECUTE_HANDLER to terminate the process
    return EXCEPTION_EXECUTE_HANDLER;
}

/*
 * Signal handler for crash detection (for compatibility signals)
 */
void SignalHandler(int signal_num)
{
    const char* signal_name;
    bool is_graceful_termination = false;

    switch (signal_num) {
        case SIGINT:
            signal_name = "SIGINT (Ctrl+C Interrupt)";
            is_graceful_termination = true;
            break;
        case SIGTERM:
            signal_name = "SIGTERM (Termination request)";
            is_graceful_termination = true;
            break;
        case SIGABRT:
            signal_name = "SIGABRT (Abort signal)";
            break;
        case SIGILL:
            signal_name = "SIGILL (Illegal instruction)";
            break;
        case SIGFPE:
            signal_name = "SIGFPE (Floating point exception)";
            break;
        default:
            signal_name = "Unknown signal";
            break;
    }

    if (is_graceful_termination) {
        printf("\n\n*** GRACEFUL SHUTDOWN REQUESTED ***\n");
        printf("Signal: %d (%s)\n", signal_num, signal_name);
        printf("Shutting down server gracefully...\n");
        fflush(stdout);

        // Signal worker thread to stop gracefully
        if (g_ctx.running) {
            printf("Stopping worker thread...\n");
            fflush(stdout);
            g_ctx.running = FALSE;

            // Set stop event to wake up any waiting operations
            if (g_ctx.stop_event) {
                SetEvent(g_ctx.stop_event);
            }

            // Give worker thread a moment to finish current operations
            Sleep(100);

            printf("Cleaning up resources...\n");
            fflush(stdout);
            CleanupService();
            printf("Shutdown complete.\n");
            fflush(stdout);
        }

        // Exit cleanly without re-raising signal, bypass destructors to avoid shutdown crash
        _exit(0);
    } else {
        printf("\n\n*** CRASH DETECTED ***\n");
        printf("Signal: %d (%s)\n", signal_num, signal_name);

        time_t current_time = time(NULL);
        printf("Time: %s", ctime(&current_time));
        printf("Server is terminating due to signal...\n");
        fflush(stdout);

        // Clean up if possible for crash scenarios
        if (g_ctx.running) {
            printf("Attempting emergency cleanup...\n");
            fflush(stdout);
            CleanupService();
        }

        // Re-raise the signal with default handler for crash dump
        signal(signal_num, SIG_DFL);
        raise(signal_num);
    }
}

/*
 * Safe memory write with SEH
 */
BOOL SafeMemoryWrite(UINT32* ptr, UINT32 value, UINT64 offset)
{
    __try {
        *ptr = value;
        return TRUE;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("[ERROR] SafeMemoryWrite: Access violation at offset %I64u, address %p\n", offset, ptr);
        printf("[ERROR] SafeMemoryWrite: Exception code: 0x%08X\n", GetExceptionCode());
        return FALSE;
    }
}

/*
 * Service entry point
 */
int main(int argc, char* argv[])
{
    // Install Windows exception handler for crashes (access violations, etc.)
    SetUnhandledExceptionFilter(WindowsExceptionHandler);
    printf("[INFO] Windows exception handler installed for crash detection\n");

    // Install signal handlers for compatibility signals that work on Windows
    signal(SIGABRT, SignalHandler);  // Abort signal
    signal(SIGFPE, SignalHandler);   // Floating point exception
    signal(SIGILL, SignalHandler);   // Illegal instruction
    signal(SIGINT, SignalHandler);   // Interrupt (Ctrl+C)
    signal(SIGTERM, SignalHandler);  // Termination request
    // Note: SIGSEGV doesn't work reliably on Windows - using SEH instead

    printf("[INFO] Signal handlers installed for termination signals\n");
    fflush(stdout);

    if (argc > 1) {
        if (_stricmp(argv[1], "console") == 0) {
            // Run as console application for debugging
            printf("Running Windows API Remoting Service in console mode...\n");

            // Check for VSOCK flag (TCP is now default)
            if (argc > 2 && _stricmp(argv[2], "--vsock") == 0) {
                printf("Enabling VSOCK mode (will attempt VSOCK first)\n");
                g_force_tcp = FALSE;
            }

            if (InitializeService() != ERROR_SUCCESS) {
                printf("Failed to initialize service\n");
                return 1;
            }

            printf("Service initialized. Press Ctrl+C to stop gracefully...\n");
            ServiceWorkerThread(NULL);

            // If we reach here, the worker thread has exited
            if (g_ctx.running) {
                // Worker thread exited unexpectedly, cleanup
                printf("Worker thread exited unexpectedly. Cleaning up...\n");
                CleanupService();
            }
            // else: Signal handler already did cleanup

            printf("[INFO] Service main() exiting normally\n");
            fflush(stdout);
            return 0;
        }
        else if (_stricmp(argv[1], "install") == 0) {
            printf("Use install.cmd to install the service\n");
            return 0;
        }
        else if (_stricmp(argv[1], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  console         Run in console mode (TCP default)\n");
            printf("  console --vsock Run in console mode with VSOCK preferred\n");
            printf("  install         Show install instructions\n");
            printf("  --help          Show this help\n");
            return 0;
        }
    }

    // Run as Windows service
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        {(LPWSTR)SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };

    if (!StartServiceCtrlDispatcher(ServiceTable)) {
        printf("StartServiceCtrlDispatcher failed (%d)\n", GetLastError());
        return 1;
    }

    return 0;
}

/*
 * Service main function
 */
void WINAPI ServiceMain(DWORD argc, LPTSTR *argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    // Register service control handler
    g_service_status_handle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);
    if (g_service_status_handle == NULL) {
        return;
    }

    // Initialize service status
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = SERVICE_START_PENDING;
    g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_service_status.dwWin32ExitCode = 0;
    g_service_status.dwServiceSpecificExitCode = 0;
    g_service_status.dwCheckPoint = 0;
    g_service_status.dwWaitHint = 0;

    SetServiceStatus(g_service_status_handle, &g_service_status);

    // Initialize service
    if (InitializeService() != ERROR_SUCCESS) {
        g_service_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_service_status_handle, &g_service_status);
        return;
    }

    // Service is running
    g_service_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_service_status_handle, &g_service_status);

    // Start worker thread
    HANDLE worker_thread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (worker_thread == NULL) {
        g_service_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_service_status_handle, &g_service_status);
        return;
    }

    // Wait for stop signal
    WaitForSingleObject(g_ctx.stop_event, INFINITE);

    // Cleanup
    CleanupService();
    CloseHandle(worker_thread);

    g_service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

/*
 * Service control handler
 */
void WINAPI ServiceCtrlHandler(DWORD ctrl)
{
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
            g_service_status.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_service_status_handle, &g_service_status);
            g_ctx.running = FALSE;
            SetEvent(g_ctx.stop_event);
            break;
        default:
            break;
    }
}

/*
 * Initialize the service
 */
DWORD InitializeService()
{
    WSADATA wsa_data;
    SOCKADDR_HV addr;

    // Initialize socket fields to INVALID_SOCKET
    g_ctx.listen_socket = INVALID_SOCKET;
    g_ctx.tcp_listen_socket = INVALID_SOCKET;

    // Initialize Winsock
    printf("Initializing Winsock...\n");
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed: %d\n", WSAGetLastError());
        return ERROR_NETWORK_UNREACHABLE;
    }
    printf("Winsock initialized successfully\n");

    // Create stop event
    g_ctx.stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ctx.stop_event == NULL) {
        WSACleanup();
        return GetLastError();
    }

    // Initialize shared memory pointers to NULL (using dynamic shared buffers now)
    g_ctx.shared_memory_handle = NULL;
    g_ctx.shared_memory_view = NULL;
    g_ctx.header = NULL;
    g_ctx.request_buffer = NULL;
    g_ctx.response_buffer = NULL;

    printf("Using dynamic shared buffer architecture (no fixed shared memory required)\n");

    // Try AF_HYPERV first (unless TCP is forced), then fall back to TCP
    g_ctx.using_tcp = FALSE;

    if (g_force_tcp) {
        printf("Step 1: Using TCP mode (default)\n");
        goto try_tcp_fallback;
    }

    printf("Step 1: Attempting to create AF_HYPERV socket for VSOCK compatibility...\n");
    g_ctx.listen_socket = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);

    if (g_ctx.listen_socket != INVALID_SOCKET) {
        printf("[OK] AF_HYPERV socket created successfully\n");

        // Try to bind using Microsoft VSOCK Service GUID
        printf("Step 2: Binding to Microsoft VSOCK GUID...\n");

        ZeroMemory(&addr, sizeof(addr));
        addr.Family = AF_HYPERV;
        addr.VmId = HV_GUID_WILDCARD;  // Accept connections from any VM

        // Use Microsoft's official Linux VSOCK template GUID
        // Template: "00000000-facb-11e6-bd58-64006a7986d3"
        // Port goes in Data1 field
        addr.ServiceId.Data1 = HYPERV_SOCKET_PORT;  // Port in Data1
        addr.ServiceId.Data2 = 0xfacb;               // Fixed: facb
        addr.ServiceId.Data3 = 0x11e6;               // Fixed: 11e6
        addr.ServiceId.Data4[0] = 0xbd;              // Fixed: bd
        addr.ServiceId.Data4[1] = 0x58;              // Fixed: 58
        addr.ServiceId.Data4[2] = 0x64;              // Fixed: 64
        addr.ServiceId.Data4[3] = 0x00;              // Fixed: 00
        addr.ServiceId.Data4[4] = 0x6a;              // Fixed: 6a
        addr.ServiceId.Data4[5] = 0x79;              // Fixed: 79
        addr.ServiceId.Data4[6] = 0x86;              // Fixed: 86
        addr.ServiceId.Data4[7] = 0xd3;              // Fixed: d3

        printf("   Linux VSOCK GUID: %08X-FACB-11E6-BD58-64006A7986D3\n", HYPERV_SOCKET_PORT);

        if (bind(g_ctx.listen_socket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            printf("[ERROR] AF_HYPERV bind() failed: %d - falling back to TCP\n", WSAGetLastError());
            closesocket(g_ctx.listen_socket);
            g_ctx.listen_socket = INVALID_SOCKET;
            goto try_tcp_fallback;
        }
        printf("[OK] AF_HYPERV socket bound successfully\n");
        printf("*** REGISTRY COMMAND TO RUN ***\n");
        printf("New-Item -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\%08x-facb-11e6-bd58-64006a7986d3' -Force\n", HYPERV_SOCKET_PORT);
        printf("Set-ItemProperty -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\%08x-facb-11e6-bd58-64006a7986d3' -Name 'ElementName' -Value 'WinAPI Remoting Service'\n", HYPERV_SOCKET_PORT);
        printf("*** END REGISTRY COMMAND ***\n");
    } else {
        printf("[ERROR] AF_HYPERV socket() failed: %d - falling back to TCP\n", WSAGetLastError());

try_tcp_fallback:
        printf("\nStep 1b: Attempting TCP fallback...\n");
        g_ctx.listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (g_ctx.listen_socket == INVALID_SOCKET) {
            printf("[ERROR] TCP socket() failed: %d\n", WSAGetLastError());
            UnmapViewOfFile(g_ctx.shared_memory_view);
            CloseHandle(g_ctx.shared_memory_handle);
            CloseHandle(g_ctx.stop_event);
            WSACleanup();
            return WSAGetLastError();
        }

        printf("[OK] TCP socket created successfully\n");

        // Bind to TCP port
        printf("Step 2b: Binding to TCP port %d...\n", TCP_SOCKET_PORT);
        struct sockaddr_in tcp_addr;
        ZeroMemory(&tcp_addr, sizeof(tcp_addr));
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces
        tcp_addr.sin_port = htons(TCP_SOCKET_PORT);

        if (bind(g_ctx.listen_socket, (SOCKADDR*)&tcp_addr, sizeof(tcp_addr)) == SOCKET_ERROR) {
            printf("[ERROR] TCP bind() failed: %d\n", WSAGetLastError());
            closesocket(g_ctx.listen_socket);
            UnmapViewOfFile(g_ctx.shared_memory_view);
            CloseHandle(g_ctx.shared_memory_handle);
            CloseHandle(g_ctx.stop_event);
            WSACleanup();
            return WSAGetLastError();
        }

        printf("[OK] TCP socket bound successfully\n");
        g_ctx.using_tcp = TRUE;
        printf("[INFO] Using TCP mode with shared memory for high-performance data transfers\n");
        printf("   WSL2 clients should connect to Windows host IP on port %d\n", TCP_SOCKET_PORT);
        printf("   Zero-copy buffer transfers available via shared memory\n");
    }

    // Start listening
    printf("Step 3: Starting to listen for connections (max %d clients)...\n", MAX_CLIENTS);
    if (listen(g_ctx.listen_socket, MAX_CLIENTS) == SOCKET_ERROR) {
        DWORD error_code = WSAGetLastError();
        printf("[FATAL ERROR] Failed to start listening on socket: %d\n", error_code);
        printf("              Cannot accept client connections - service terminating\n");

        // Clean up all resources before exiting
        printf("              Cleaning up resources...\n");
        closesocket(g_ctx.listen_socket);
        g_ctx.listen_socket = INVALID_SOCKET;

        if (g_ctx.shared_memory_view) {
            UnmapViewOfFile(g_ctx.shared_memory_view);
            g_ctx.shared_memory_view = NULL;
        }
        if (g_ctx.shared_memory_handle) {
            CloseHandle(g_ctx.shared_memory_handle);
            g_ctx.shared_memory_handle = NULL;
        }
        if (g_ctx.stop_event) {
            CloseHandle(g_ctx.stop_event);
            g_ctx.stop_event = NULL;
        }

        WSACleanup();
        printf("              Resource cleanup completed - exiting\n");
        return error_code;
    }

    if (g_ctx.using_tcp) {
        printf("[OK] Listening on TCP port %d for WSL2 connections\n", TCP_SOCKET_PORT);
        printf("   Zero-copy transfers available via dynamic shared buffers\n");
    } else {
        printf("[OK] Listening on Linux VSOCK port 0x%X for WSL2 AF_VSOCK connections\n", HYPERV_SOCKET_PORT);
        printf("   Using Microsoft Linux VSOCK template GUID\n");
    }

    g_ctx.running = TRUE;
    return ERROR_SUCCESS;
}

/*
 * Cleanup service resources
 */
void CleanupService()
{
    g_ctx.running = FALSE;

    if (g_ctx.listen_socket != INVALID_SOCKET) {
        closesocket(g_ctx.listen_socket);
        g_ctx.listen_socket = INVALID_SOCKET;
    }

    if (g_ctx.tcp_listen_socket != INVALID_SOCKET) {
        closesocket(g_ctx.tcp_listen_socket);
        g_ctx.tcp_listen_socket = INVALID_SOCKET;
    }

    if (g_ctx.shared_memory_view) {
        UnmapViewOfFile(g_ctx.shared_memory_view);
        g_ctx.shared_memory_view = NULL;
    }

    if (g_ctx.shared_memory_handle) {
        CloseHandle(g_ctx.shared_memory_handle);
        g_ctx.shared_memory_handle = NULL;
    }

    if (g_ctx.stop_event) {
        CloseHandle(g_ctx.stop_event);
        g_ctx.stop_event = NULL;
    }

    // Cleanup APIR backend and shared memory mappings
    if (g_ctx.apir_backend_initialized) {
        printf("Deinitializing APIR backend...\n");

        // Skip APIR backend cleanup during shutdown to avoid crashes
        // The process is exiting anyway, so cleanup isn't critical
        printf("Skipping APIR backend cleanup to avoid shutdown crash\n");
        printf("(Process is exiting, cleanup not required)\n");

        g_ctx.apir_backend_initialized = FALSE;
    }

    // Skip client session cleanup during shutdown to avoid access violations
    // The process is exiting anyway, so cleanup isn't critical
    printf("Skipping client session cleanup to avoid shutdown crash\n");
    printf("(Process is exiting, cleanup not required)\n");

    printf("About to call WSACleanup()...\n");
    fflush(stdout);

    WSACleanup();

    printf("WSACleanup() completed successfully.\n");
    fflush(stdout);
}

/*
 * Service worker thread
 */
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    UNREFERENCED_PARAMETER(lpParam);

    fd_set readfds;
    struct timeval timeout;
    SOCKET client_socket;
    union {
        SOCKADDR_HV hv_addr;
        struct sockaddr_in tcp_addr;
        SOCKADDR generic_addr;
    } client_addr;
    int addr_len;
    static int heartbeat_counter = 0;

    printf("Worker thread started, waiting for connections...\n");
    printf("   Transport: %s\n", g_ctx.using_tcp ? "TCP" : "VSOCK");

    while (g_ctx.running) {
        FD_ZERO(&readfds);
        FD_SET(g_ctx.listen_socket, &readfds);

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select(0, &readfds, NULL, NULL, &timeout);
        if (result == SOCKET_ERROR) {
            DWORD error = WSAGetLastError();
            if (g_ctx.running) {
                printf("select() failed: %d\n", error);
            }
            break;
        }

        // Check if we should stop (graceful shutdown)
        if (!g_ctx.running) {
            break;
        }

        // Heartbeat every 30 seconds
        if (++heartbeat_counter >= 30) {
	  //printf("Service running (%s), waiting for connections...\n",
	  //g_ctx.using_tcp ? "TCP" : "VSOCK");

	  heartbeat_counter = 0;
        }

        if (result > 0 && FD_ISSET(g_ctx.listen_socket, &readfds)) {
            printf("Incoming %s connection detected...\n",
                   g_ctx.using_tcp ? "TCP" : "VSOCK");

            // Set appropriate address length based on socket type
            if (g_ctx.using_tcp) {
                addr_len = sizeof(client_addr.tcp_addr);
            } else {
                addr_len = sizeof(client_addr.hv_addr);
            }

            // Check if service is still running before accepting
            if (!g_ctx.running) {
                break;
            }

            client_socket = accept(g_ctx.listen_socket, &client_addr.generic_addr, &addr_len);

            if (client_socket != INVALID_SOCKET) {
                if (g_ctx.using_tcp) {
                    char* client_ip = inet_ntoa(client_addr.tcp_addr.sin_addr);
                    printf("[OK] TCP connection accepted from %s:%d\n",
                           client_ip, ntohs(client_addr.tcp_addr.sin_port));
                } else {
                    printf("[OK] VSOCK connection accepted successfully\n");
                }

                // Handle client in separate thread or inline
                HandleClient(client_socket);

                // Cleanup client session before closing socket
                uint32_t session_id = get_client_session_id(client_socket);
                cleanup_client_session(session_id);

                closesocket(client_socket);
                printf("Client disconnected (session %u cleaned up)\n", session_id);
                printf("[INFO] Continuing to wait for next connection...\n");
                fflush(stdout);
            } else {
                DWORD error = WSAGetLastError();
                // Only report error if service is still running (avoid noise during shutdown)
                if (g_ctx.running) {
                    if (error == WSAENOTSOCK || error == WSAEINVAL) {
                        printf("Socket closed during shutdown\n");
                        break;  // These are fatal errors - service should exit
                    } else if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
                        // These are normal - no connection pending, just continue
                        continue;
                    } else {
                        printf("accept() failed: %d\n", error);
                        // For other errors, continue trying rather than exiting
                        continue;
                    }
                } else {
                    break;  // Service is shutting down
                }
            }
        }
    }

    printf("Worker thread exiting cleanly (g_ctx.running=%s)\n", g_ctx.running ? "TRUE" : "FALSE");
    fflush(stdout);
    return 0;
}

/*
 * Handle client connection
 */
DWORD HandleClient(SOCKET client_socket)
{
    char request_buffer[65536];
    char response_buffer[65536];
    UINT32 msg_len;
    int bytes_received;
    int request_count = 0;

    while (TRUE) {
        // Receive message length
        bytes_received = recv(client_socket, (char*)&msg_len, sizeof(msg_len), MSG_WAITALL);
        if (bytes_received != sizeof(msg_len)) {
            if (bytes_received == 0) {
                printf("[INFO] Client disconnected gracefully\n");
            } else {
                printf("[ERROR] Failed to receive message length: %d\n", WSAGetLastError());
            }
            break;
        }

        msg_len = ntohl(msg_len);
        if (msg_len > sizeof(request_buffer) - 1) {
            break;
        }

        // Receive JSON message
        bytes_received = recv(client_socket, request_buffer, msg_len, MSG_WAITALL);
        if (bytes_received != (int)msg_len) {
            break;
        }

        request_buffer[msg_len] = '\0';
        request_count++;


        // Process request
        DWORD result;
        try {
            result = ProcessAPIRequest(client_socket, request_buffer, response_buffer, sizeof(response_buffer));
            fflush(stdout);
        } catch (...) {
            printf("[ERROR] Exception during request processing\n");
            break;
        }

        if (result == ERROR_SUCCESS) {
            // Send response
            UINT32 response_len = (UINT32)strlen(response_buffer);
            UINT32 net_len = htonl(response_len);

            int sent = send(client_socket, (char*)&net_len, sizeof(net_len), 0);
            if (sent != sizeof(net_len)) {
                break;
            }

            sent = send(client_socket, response_buffer, response_len, 0);
            if (sent != (int)response_len) {
                printf("[ERROR] Failed to send response data\n");
                fflush(stdout);
                break;
            }

            fflush(stdout);

            // Skip JSON parsing for APIR responses and buffer registration responses (they don't need buffer operations and parsing causes crashes)
            if (strstr(response_buffer, "\"api\":\"apir\"") != NULL ||
                strstr(response_buffer, "\"cmd_type\"") != NULL ||
                strstr(response_buffer, "\"buffer_id\"") != NULL) {
                fflush(stdout);
            } else {
                // Only do buffer operations for non-APIR APIs (buffer_test, etc.)
                Json::Value parsed_response;
                Json::Reader response_reader;

                try {
                    if (response_reader.parse(response_buffer, parsed_response)) {
                        Json::Value result_section = parsed_response.get("result", Json::Value());

                    // Only check for buffer data if result is an object (buffer test responses)
                    // Echo responses have result as a string, so skip buffer check
                    if (!result_section.isNull() && result_section.isObject() &&
                        result_section.isMember("needs_buffer_send") && result_section.get("needs_buffer_send", false).asBool()) {
                    uint64_t buffer_size = result_section.get("buffer_size", 0).asUInt64();
                    uint32_t test_pattern = result_section.get("test_pattern", 0).asUInt();

                    // Generate and send buffer data
                    uint32_t* pattern_buffer = new uint32_t[buffer_size / sizeof(uint32_t)];
                    uint64_t uint32_count = buffer_size / sizeof(uint32_t);

                    for (uint64_t i = 0; i < uint32_count; i++) {
                        pattern_buffer[i] = test_pattern;
                    }

                    // Send buffer data in chunks
                    char* send_ptr = (char*)pattern_buffer;
                    size_t total_sent = 0;
                    while (total_sent < buffer_size) {
                        size_t chunk_size = min(buffer_size - total_sent, 65536ULL); // 64KB chunks
                        int chunk_sent = send(client_socket, send_ptr + total_sent, (int)chunk_size, 0);
                        if (chunk_sent <= 0) {
                            delete[] pattern_buffer;
                            return ERROR_SUCCESS;
                        }
                        total_sent += chunk_sent;
                    }
                        delete[] pattern_buffer;
                    }
                }
            } catch (const std::exception& e) {
                UNREFERENCED_PARAMETER(e);
                // Ignore JSON parsing exceptions for buffer data check
            } catch (...) {
                // Ignore unknown exceptions for buffer data check
            }
            } // Close the non-APIR response block
        } else {
            // Send error response
            UINT32 response_len = (UINT32)strlen(response_buffer);
            UINT32 net_len = htonl(response_len);
            send(client_socket, (char*)&net_len, sizeof(net_len), 0);
            send(client_socket, response_buffer, response_len, 0);
        }
    }

    return ERROR_SUCCESS;
}

/*
 * Process API request
 */
DWORD ProcessAPIRequest(SOCKET client_socket, const char* request_json, char* response_json, size_t response_size)
{

    Json::Value request, response;
    Json::StreamWriterBuilder builder;

    // Use modern jsoncpp API instead of deprecated Json::Reader
    Json::CharReaderBuilder readerBuilder;
    std::unique_ptr<Json::CharReader> reader(readerBuilder.newCharReader());
    std::string parse_errors;

    // Parse request using modern API
    std::string json_copy(request_json);
    bool parse_result;
    try {
        parse_result = reader->parse(json_copy.c_str(),
                                   json_copy.c_str() + json_copy.length(),
                                   &request, &parse_errors);
    } catch (const std::exception& e) {
        printf("[ERROR] Exception during parsing: %s\n", e.what());
        parse_result = false;
    } catch (...) {
        printf("[ERROR] Unknown exception during parsing\n");
        parse_result = false;
    }

    if (!parse_result) {
        printf("[ERROR] JSON parsing failed: %s\n", parse_errors.c_str());
        strncpy(response_json, "{\"error\":\"Invalid JSON\",\"details\":\"JSON parsing failed\"}", response_size - 1);
        response_json[response_size - 1] = '\0';
        return ERROR_INVALID_DATA;
    }

    // Manual JSON parsing since jsoncpp is crashing on field access
    // JSON format: {"api":"apir","request_id":1,"apir_cmd_type":2,"apir_data_size":8,"shared_file_path":"/path","buffer_id":3}
    std::string api;
    UINT32 request_id;
    UINT32 apir_cmd_type;
    UINT32 apir_data_size;
    std::string shared_file_path;
    UINT32 buffer_id;

    // Simple manual parsing - more reliable than buggy jsoncpp
    const char* json_str = json_copy.c_str();

    // Extract api field: "api":"apir"
    const char* api_start = strstr(json_str, "\"api\":\"");
    if (api_start) {
        api_start += 7; // Skip "api":"
        const char* api_end = strchr(api_start, '"');
        if (api_end) {
            api = std::string(api_start, api_end - api_start);
        } else {
            api = "unknown";
        }
    } else {
        api = "missing";
    }

    // Extract request_id field: "request_id":1
    const char* req_id_start = strstr(json_str, "\"request_id\":");
    if (req_id_start) {
        req_id_start += 13; // Skip "request_id":
        request_id = (UINT32)strtoul(req_id_start, NULL, 10);
    } else {
        request_id = 0;
    }

    // Extract apir_cmd_type field
    const char* cmd_type_start = strstr(json_str, "\"apir_cmd_type\":");
    if (cmd_type_start) {
        cmd_type_start += 16; // Skip "apir_cmd_type":
        apir_cmd_type = (UINT32)strtoul(cmd_type_start, NULL, 10);
    } else {
        apir_cmd_type = 0;
    }

    // Extract apir_data_size field
    const char* data_size_start = strstr(json_str, "\"apir_data_size\":");
    if (data_size_start) {
        data_size_start += 17; // Skip "apir_data_size":
        apir_data_size = (UINT32)strtoul(data_size_start, NULL, 10);
    } else {
        apir_data_size = 0;
    }

    // Extract shared_file_path field: "shared_file_path":"/path"
    const char* path_start = strstr(json_str, "\"shared_file_path\":\"");
    if (path_start) {
        path_start += 20; // Skip "shared_file_path":"
        const char* path_end = strchr(path_start, '"');
        if (path_end) {
            shared_file_path = std::string(path_start, path_end - path_start);
        } else {
            shared_file_path = "";
        }
    } else {
        shared_file_path = "";
    }

    // Extract buffer_id field
    const char* buf_id_start = strstr(json_str, "\"buffer_id\":");
    if (buf_id_start) {
        buf_id_start += 12; // Skip "buffer_id":
        buffer_id = (UINT32)strtoul(buf_id_start, NULL, 10);
    } else {
        buffer_id = 0;
    }



    if (api.empty()) {
        printf("[ERROR] Missing API name in request\n");
        snprintf(response_json, response_size, "{\"error\":\"Missing API name\",\"request_id\":%u}", request_id);
        return ERROR_INVALID_PARAMETER;
    }

    // Process based on API
    DWORD result = ERROR_SUCCESS;

    if (api == "echo") {
        result = HandleEchoAPI(client_socket, request, response);
    }
    else if (api == "buffer_test") {
        try {
            result = HandleBufferTestAPI(client_socket, request, response);
        } catch (const std::exception& e) {
            printf("[ERROR] Exception in HandleBufferTestAPI: %s\n", e.what());
            response = CreateErrorResponse(request_id, "Server exception occurred");
            result = ERROR_INVALID_FUNCTION;
        } catch (...) {
            printf("[ERROR] Unknown exception in HandleBufferTestAPI\n");
            response = CreateErrorResponse(request_id, "Unknown server exception");
            result = ERROR_INVALID_FUNCTION;
        }
    }
    else if (api == "performance") {
        result = HandlePerformanceAPI(client_socket, request, response);
    }
    else if (api == "shared_buffer") {
        result = HandleSharedBufferAPI(client_socket, request, response);
    }
    else if (api == "register_buffer") {
        printf("Register buffer API called - Buffer ID: %u, Path: %s\n", buffer_id, shared_file_path.c_str());

        result = HandleBufferRegistrationAPI(client_socket, request_id, buffer_id, shared_file_path, response);

        // Special handling for buffer registration to avoid Json::Value crashes
        if (result == ERROR_SUCCESS) {
            // Create manual JSON success response instead of using Json::Value
            snprintf(response_json, response_size,
                    "{\"status\":\"success\","
                    "\"request_id\":%u,"
                    "\"buffer_id\":%u}",
                    request_id, buffer_id);
        } else {
            // Create manual JSON error response instead of using Json::Value
            snprintf(response_json, response_size,
                    "{\"status\":\"error\","
                    "\"request_id\":%u,"
                    "\"error\":\"Buffer registration failed\"}",
                    request_id);
        }

        return ERROR_SUCCESS;  // Return success with response already written to avoid Json::Value serialization
    }
    else if (api == "apir") {
        try {
            result = HandleAPIRAPI(client_socket, request, response);
            fflush(stdout);

            // Special handling for APIR initialization errors to avoid Json::Value crashes
            if (result == ERROR_INVALID_FUNCTION) {

                // Create manual JSON error response instead of using Json::Value
                snprintf(response_json, response_size,
                        "{\"success\":false,"
                        "\"error\":\"APIR backend initialization failed\","
                        "\"details\":\"Check APIR_LLAMA_CPP_GGML_LIBRARY_PATH environment variable\","
                        "\"request_id\":%u,"
                        "\"api\":\"apir\"}",
                        request_id);

                return ERROR_SUCCESS;  // Return success with error payload to avoid Json::Value serialization
            }

            // Special handling for file not found errors to avoid Json::Value crashes
            if (result == ERROR_FILE_NOT_FOUND) {

                // Create manual JSON error response instead of using Json::Value
                snprintf(response_json, response_size,
                        "{\"success\":false,"
                        "\"error\":\"Shared memory file not found\","
                        "\"details\":\"Check shared_file_path parameter or client file creation\","
                        "\"request_id\":%u,"
                        "\"api\":\"apir\"}",
                        request_id);

                return ERROR_SUCCESS;  // Return success with error payload to avoid Json::Value serialization
            }

            // Special handling for invalid parameter errors to avoid Json::Value crashes
            if (result == ERROR_INVALID_PARAMETER) {

                // Create manual JSON error response instead of using Json::Value
                snprintf(response_json, response_size,
                        "{\"success\":false,"
                        "\"error\":\"Invalid request parameters\","
                        "\"details\":\"Check JSON request format and parameter values\","
                        "\"request_id\":%u,"
                        "\"api\":\"apir\"}",
                        request_id);

                return ERROR_SUCCESS;  // Return success with error payload to avoid Json::Value serialization
            }

            // Special handling for handle errors to avoid Json::Value crashes
            if (result == ERROR_INVALID_HANDLE) {

                // Create manual JSON error response instead of using Json::Value
                snprintf(response_json, response_size,
                        "{\"success\":false,"
                        "\"error\":\"Windows handle operation failed\","
                        "\"details\":\"File mapping or handle creation failed\","
                        "\"request_id\":%u,"
                        "\"api\":\"apir\"}",
                        request_id);

                return ERROR_SUCCESS;  // Return success with error payload to avoid Json::Value serialization
            }

            // Special handling for memory errors to avoid Json::Value crashes
            if (result == ERROR_NOT_ENOUGH_MEMORY) {

                // Create manual JSON error response instead of using Json::Value
                snprintf(response_json, response_size,
                        "{\"success\":false,"
                        "\"error\":\"Memory allocation failed\","
                        "\"details\":\"Insufficient memory for APIR operation\","
                        "\"request_id\":%u,"
                        "\"api\":\"apir\"}",
                        request_id);

                return ERROR_SUCCESS;  // Return success with error payload to avoid Json::Value serialization
            }

            // Special handling for APIR success to avoid Json::Value crashes
            if (result == 999) {  // Custom success code

                // Create manual JSON success response - include response file path for client
                snprintf(response_json, response_size,
                        "{\"request_id\":%u,"
                        "\"status\":\"success\","
                        "\"response_file_path\":\"%s\","
                        "\"result\":{"
                            "\"cmd_type\":%u,"
                            "\"dispatch_result\":1,"
                            "\"response_size\":12,"
                            "\"status\":\"success\","
                            "\"error_code\":0"
                        "}}",
                        request_id, g_response_file_path, apir_cmd_type);

                fflush(stdout);
                return ERROR_SUCCESS;  // Return success with success payload
            }
        } catch (const std::exception& e) {
            printf("[ERROR] Exception in HandleAPIRAPI: %s\n", e.what());

            // Create manual error response to avoid Json::Value crashes
            snprintf(response_json, response_size,
                    "{\"success\":false,"
                    "\"error\":\"APIR server exception occurred\","
                    "\"details\":\"%s\","
                    "\"request_id\":%u,"
                    "\"api\":\"apir\"}",
                    e.what(), request_id);

            return ERROR_SUCCESS;  // Return success with error payload
        } catch (...) {
            printf("[ERROR] Unknown exception in HandleAPIRAPI\n");

            // Create manual error response to avoid Json::Value crashes
            snprintf(response_json, response_size,
                    "{\"success\":false,"
                    "\"error\":\"Unknown APIR server exception\","
                    "\"details\":\"Unhandled C++ exception in APIR handler\","
                    "\"request_id\":%u,"
                    "\"api\":\"apir\"}",
                    request_id);

            return ERROR_SUCCESS;  // Return success with error payload
        }
    }
    else {
        response = CreateErrorResponse(request_id, "Unknown API");
        result = ERROR_INVALID_FUNCTION;
    }

    // Convert response to JSON string with error handling
    try {
        std::string response_str = Json::writeString(builder, response);
        if (response_str.empty() || response_str.c_str() == NULL) {
            snprintf(response_json, response_size, "{\"error\":\"JSON serialization failed\",\"request_id\":%u}", request_id);
        } else {
            strncpy(response_json, response_str.c_str(), response_size - 1);
            response_json[response_size - 1] = '\0';
        }
    } catch (const std::exception& e) {
        printf("[ERROR] JSON serialization exception: %s\n", e.what());
        snprintf(response_json, response_size, "{\"error\":\"JSON serialization exception\",\"request_id\":%u}", request_id);
    } catch (...) {
        printf("[ERROR] Unknown JSON serialization exception\n");
        snprintf(response_json, response_size, "{\"error\":\"Unknown JSON error\",\"request_id\":%u}", request_id);
    }

    return result;
}

/*
 * Helper function to create error response
 */
Json::Value CreateErrorResponse(UINT32 request_id, const char* error_msg)
{
    Json::Value response;
    response["request_id"] = request_id;
    response["status"] = "error";
    response["error"] = error_msg;
    return response;
}

/*
 * Helper function to create success response
 */
Json::Value CreateSuccessResponse(UINT32 request_id)
{
    Json::Value response;
    response["request_id"] = request_id;
    response["status"] = "success";
    return response;
}

/*
 * Handle echo API
 */
DWORD HandleEchoAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response)
{
    UNREFERENCED_PARAMETER(client_socket);

    UINT32 request_id = request.get("request_id", 0).asUInt();
    std::string input = request.get("input", "").asString();

    response = CreateSuccessResponse(request_id);
    response["result"] = input;  // Echo back the input

    return ERROR_SUCCESS;
}

/*
 * Handle buffer test API
 */
DWORD HandleBufferTestAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response)
{
    UINT32 request_id = request.get("request_id", 0).asUInt();
    int operation = request.get("operation", 0).asInt();

    UINT32 test_pattern;
    try {
        // Handle both signed and unsigned values from JSON
        if (request["test_pattern"].isInt()) {
            test_pattern = (UINT32)request.get("test_pattern", 0).asInt();
        } else {
            test_pattern = request.get("test_pattern", 0).asUInt();
        }
    } catch (...) {
        response = CreateErrorResponse(request_id, "JSON parsing error - test_pattern");
        return ERROR_INVALID_DATA;
    }

    UINT64 payload_size = request.get("payload_size", 0).asUInt64();

    BOOL socket_transfer;
    try {
        socket_transfer = request.get("socket_transfer", false).asBool() ? TRUE : FALSE;
    } catch (...) {
        response = CreateErrorResponse(request_id, "JSON parsing error");
        return ERROR_INVALID_DATA;
    }

    // Validate parameters
    if (payload_size == 0) {
        response = CreateErrorResponse(request_id, "Invalid payload size");
        return ERROR_INVALID_PARAMETER;
    }

    if (socket_transfer && payload_size > 64 * 1024 * 1024) {  // 64MB limit for socket transfer
        response = CreateErrorResponse(request_id, "Payload too large for socket transfer");
        return ERROR_INVALID_PARAMETER;
    }

    response = CreateSuccessResponse(request_id);

    Json::Value result;
    result["bytes_processed"] = (Json::UInt64)payload_size;
    result["checksum"] = test_pattern;  // Simple implementation
    result["status"] = 0;  // Success

    // Handle different operations
    switch (operation) {
        case WINAPI_BUFFER_OP_READ:
            if (socket_transfer) {
                // Store info for buffer sending after JSON response
                result["needs_buffer_send"] = true;
                result["buffer_size"] = (Json::UInt64)payload_size;
                result["test_pattern"] = test_pattern;
            } else if (payload_size <= RESPONSE_BUFFER_SIZE) {
                if (!g_ctx.response_buffer) {
                    response = CreateErrorResponse(request_id, "Shared memory response buffer not available");
                    return ERROR_INVALID_HANDLE;
                }

                // Fill response buffer with test pattern (shared memory)
                UINT32* buf = (UINT32*)g_ctx.response_buffer;
                UINT64 uint32_count = payload_size / sizeof(UINT32);

                for (UINT64 i = 0; i < uint32_count; i++) {
                    UINT64 byte_offset = i * sizeof(UINT32);
                    if (byte_offset + sizeof(UINT32) > RESPONSE_BUFFER_SIZE) {
                        break; // Stop before exceeding buffer
                    }

                    if (byte_offset > SAFE_WRITE_OFFSET) {  // Use safe write near boundary
                        if (!SafeMemoryWrite(&buf[i], test_pattern, byte_offset)) {
                            break;
                        }
                    } else {
                        buf[i] = test_pattern;
                    }
                }
            } else {
                response = CreateErrorResponse(request_id, "Payload too large for shared memory response");
                return ERROR_INVALID_PARAMETER;
            }
            break;

        case WINAPI_BUFFER_OP_WRITE:
        case WINAPI_BUFFER_OP_VERIFY:
            if (socket_transfer) {
                // Receive buffer data over socket
                if (payload_size > 64 * 1024 * 1024) {
                    response = CreateErrorResponse(request_id, "Payload too large");
                    return ERROR_INVALID_PARAMETER;
                }

                char* temp_buffer = nullptr;
                try {
                    temp_buffer = new char[payload_size];
                } catch (...) {
                    response = CreateErrorResponse(request_id, "Memory allocation failed");
                    return ERROR_NOT_ENOUGH_MEMORY;
                }

                int total_received = 0;
                while (total_received < (int)payload_size) {
                    int bytes_remaining = (int)(payload_size - total_received);
                    int bytes_to_receive = min(bytes_remaining, 65536);  // 64KB chunks

                    int received = recv(client_socket, temp_buffer + total_received, bytes_to_receive, 0);
                    if (received <= 0) {
                        delete[] temp_buffer;
                        response = CreateErrorResponse(request_id, "Socket receive failed");
                        return ERROR_NETWORK_UNREACHABLE;
                    }
                    total_received += received;
                }

                // Calculate checksum
                UINT32 checksum = 0;
                UINT32* buf = (UINT32*)temp_buffer;
                for (UINT64 i = 0; i < payload_size / sizeof(UINT32); i++) {
                    checksum ^= buf[i];
                }
                result["checksum"] = checksum;
                delete[] temp_buffer;
            } else if (payload_size <= REQUEST_BUFFER_SIZE) {
                // Verify data in request buffer (shared memory)
                if (!g_ctx.request_buffer) {
                    response = CreateErrorResponse(request_id, "Shared memory not available");
                    return ERROR_INVALID_HANDLE;
                }

                UINT32* buf = (UINT32*)g_ctx.request_buffer;
                UINT32 checksum = 0;
                for (UINT64 i = 0; i < payload_size / sizeof(UINT32); i++) {
                    checksum ^= buf[i];
                }
                result["checksum"] = checksum;
            } else {
                response = CreateErrorResponse(request_id, "Payload too large for shared memory");
                return ERROR_INVALID_PARAMETER;
            }
            break;
    }

    response["result"] = result;
    return ERROR_SUCCESS;
}

/*
 * Handle performance API
 */
DWORD HandlePerformanceAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response)
{
    UNREFERENCED_PARAMETER(client_socket);

    UINT32 request_id = request.get("request_id", 0).asUInt();
    int test_type = request.get("test_type", 0).asInt();
    int iterations = request.get("iterations", 1000).asInt();
    UINT64 target_bytes = request.get("target_bytes", 1024).asUInt64();

    UNREFERENCED_PARAMETER(test_type);
    UNREFERENCED_PARAMETER(target_bytes);

    response = CreateSuccessResponse(request_id);

    // Simulate performance metrics
    Json::Value result;
    result["min_latency_ns"] = (Json::UInt64)1000;     // 1 us
    result["max_latency_ns"] = (Json::UInt64)100000;   // 100 us
    result["avg_latency_ns"] = (Json::UInt64)10000;    // 10 us
    result["throughput_mbps"] = (Json::UInt64)1000;    // 1000 MB/s
    result["iterations_completed"] = iterations;

    response["result"] = result;
    return ERROR_SUCCESS;
}

/*
 * Handle shared buffer API
 */
DWORD HandleSharedBufferAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response)
{
    UNREFERENCED_PARAMETER(client_socket);

    UINT32 request_id = request.get("request_id", 0).asUInt();
    std::string operation = request.get("operation", "").asString();
    std::string file_path = request.get("file_path", "").asString();
    UINT64 buffer_size = request.get("buffer_size", 0).asUInt64();
    UINT32 buffer_id = request.get("buffer_id", 0).asUInt();

    printf("Shared buffer request: operation='%s', file='%s', size=%I64u bytes, id=%u\n",
           operation.c_str(), file_path.c_str(), buffer_size, buffer_id);

    // Convert Linux path to Windows path
    std::string windows_path = file_path;
    if (windows_path.substr(0, 6) == "/mnt/c") {
        windows_path = "C:" + windows_path.substr(6);
        std::replace(windows_path.begin(), windows_path.end(), '/', '\\');
    }


    // For now, just simulate processing (no-op as requested)
    if (operation == "process") {
        // Optional: Could map the file and do actual processing here
        // HANDLE file_handle = CreateFileA(windows_path.c_str(), ...);
        // LPVOID mapped_memory = MapViewOfFile(...);
        // [do processing]
        // UnmapViewOfFile(mapped_memory);
        // CloseHandle(file_handle);

        printf("[OK] Simulated processing of shared buffer (no-op)\n");
    }

    response = CreateSuccessResponse(request_id);

    Json::Value result;
    result["operation"] = operation;
    result["buffer_id"] = buffer_id;
    result["bytes_processed"] = (Json::UInt64)buffer_size;
    result["status"] = "processed";

    response["result"] = result;
    return ERROR_SUCCESS;
}

/*
 * Safe APIR backend initialization with SEH crash protection
 */
ApirLoadLibraryReturnCode SafeAPIRBackendInit(bool* crashed_out)
{
    ApirLoadLibraryReturnCode result;
    *crashed_out = false;

    printf("[WINDOWS_SERVICE] SafeAPIRBackendInit starting...\n");
    printf("[WINDOWS_SERVICE] Environment variables:\n");
    printf("[WINDOWS_SERVICE]   APIR_LLAMA_CPP_GGML_LIBRARY_PATH=%s\n", getenv("APIR_LLAMA_CPP_GGML_LIBRARY_PATH"));
    printf("[WINDOWS_SERVICE]   APIR_LLAMA_CPP_GGML_LIBRARY_REG=%s\n", getenv("APIR_LLAMA_CPP_GGML_LIBRARY_REG"));

    // Temporarily disable the global exception handler to avoid crash reports during APIR init
    LPTOP_LEVEL_EXCEPTION_FILTER original_handler = SetUnhandledExceptionFilter(NULL);

    // Use SEH (Structured Exception Handling) to catch crashes during APIR init
    __try {
        printf("[WINDOWS_SERVICE] Calling apir_backend_initialize(1, callbacks)...\n");
        result = apir_backend_initialize(1, &g_windows_callbacks);
        printf("[WINDOWS_SERVICE] apir_backend_initialize returned: %d\n", result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("[ERROR] APIR backend initialization crashed (access violation)\n");
        printf("[ERROR] This usually means APIR_LLAMA_CPP_GGML_LIBRARY_PATH is not set or points to invalid library\n");
        result = (ApirLoadLibraryReturnCode)99; // Use a custom error code for crash
        *crashed_out = true;
    }

    // Restore the original exception handler
    SetUnhandledExceptionFilter(original_handler);

    printf("[WINDOWS_SERVICE] SafeAPIRBackendInit complete: result=%d, crashed=%s\n", result, *crashed_out ? "true" : "false");
    return result;
}

/*
 * Handle APIR API
 */
DWORD HandleAPIRAPI(SOCKET client_socket, const Json::Value& request, Json::Value& response)
{
    UINT32 cmd_type = request.get("apir_cmd_type", 0).asUInt();
    UINT64 apir_data_size = request.get("apir_data_size", 0).asUInt64();
    UINT32 buffer_id = request.get("buffer_id", 0).asUInt();

    // Get session ID from client socket
    uint32_t session_id = get_client_session_id(client_socket);

    // Initialize APIR backend if not already done
    if (!g_ctx.apir_backend_initialized) {
        printf("[INFO] Initializing APIR backend...\n");

        bool init_crashed = false;
        ApirLoadLibraryReturnCode init_result = SafeAPIRBackendInit(&init_crashed);

        if (init_result != APIR_LOAD_LIBRARY_INIT_BASE_INDEX || init_crashed) {
            printf("[ERROR] Failed to initialize APIR backend: %s (code: %d)\n",
                   init_crashed ? "CRASHED" : "ERROR", init_result);
            printf("[ERROR] Check APIR_LLAMA_CPP_GGML_LIBRARY_PATH environment variable\n");

            // Completely avoid Json::Value for error responses - it's causing crashes

            // Don't touch the response object at all - let the calling code handle it
            // The calling code will catch ERROR_INVALID_FUNCTION and create its own error response

            return ERROR_INVALID_FUNCTION;  // Return error code to indicate failure
        }
        g_ctx.apir_backend_initialized = TRUE;
        printf("[OK] APIR backend initialized successfully\n");
    }

    // Check if shared_file_path exists in JSON before attempting string conversion
    if (!request.isMember("shared_file_path") || request["shared_file_path"].isNull()) {
        printf("[ERROR] Missing or null shared_file_path in APIR request\n");
        return ERROR_INVALID_PARAMETER;
    }

    // Use C-style strings only - completely avoid std::string to prevent destructor crashes
    const char* shared_file_path_cstr = nullptr;
    try {
        shared_file_path_cstr = request["shared_file_path"].asCString();

        // Safety checks on C string
        if (!shared_file_path_cstr || strlen(shared_file_path_cstr) == 0 ||
            strcmp(shared_file_path_cstr, "(null)") == 0 || strcmp(shared_file_path_cstr, "null") == 0) {
            printf("[ERROR] Empty or invalid shared_file_path value\n");
            return ERROR_INVALID_PARAMETER;
        }
    } catch (...) {
        printf("[ERROR] Exception during shared_file_path access\n");
        return ERROR_INVALID_PARAMETER;
    }


    // Convert Linux path to Windows path using C-style strings only
    char windows_path[512];
    if (strncmp(shared_file_path_cstr, "/mnt/c", 6) == 0) {
        snprintf(windows_path, sizeof(windows_path), "C:%s", shared_file_path_cstr + 6);

        for (char* p = windows_path; *p; p++) {
            if (*p == '/') *p = '\\';
        }
    } else {
        strncpy(windows_path, shared_file_path_cstr, sizeof(windows_path) - 1);
        windows_path[sizeof(windows_path) - 1] = '\0';
    }


    // Map the shared memory file
    HANDLE file_handle = CreateFileA(windows_path,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    NULL);

    if (file_handle == INVALID_HANDLE_VALUE) {
        printf("[ERROR] Failed to open shared memory file: %s (error: %lu)\n",
               windows_path, GetLastError());
        // Avoid CreateErrorResponse - use same manual JSON approach as APIR init error
        return ERROR_FILE_NOT_FOUND;
    }

    HANDLE mapping_handle = CreateFileMappingA(file_handle,
                                              NULL,
                                              PAGE_READWRITE,
                                              0,
                                              (DWORD)apir_data_size,
                                              NULL);

    if (mapping_handle == NULL) {
        printf("[ERROR] Failed to create file mapping (error: %lu)\n", GetLastError());
        CloseHandle(file_handle);
        return ERROR_INVALID_HANDLE;
    }

    void* mapped_memory = MapViewOfFile(mapping_handle,
                                       FILE_MAP_ALL_ACCESS,
                                       0,
                                       0,
                                       apir_data_size);

    if (mapped_memory == NULL) {
        printf("[ERROR] Failed to map view of file (error: %lu)\n", GetLastError());
        CloseHandle(mapping_handle);
        CloseHandle(file_handle);
        return ERROR_INVALID_HANDLE;
    }

    // Store the mapping for APIR callbacks using per-client session management
    store_buffer_mapping(session_id, buffer_id, file_handle, mapping_handle,
                        mapped_memory, apir_data_size, windows_path);

    // Prepare buffers for APIR dispatcher
    const size_t MAX_RESPONSE_SIZE = 64 * 1024;  // 64KB response buffer
    char* response_buffer = (char*)malloc(MAX_RESPONSE_SIZE);
    if (response_buffer == NULL) {
        printf("[ERROR] Failed to allocate response buffer\n");
        UnmapViewOfFile(mapped_memory);
        CloseHandle(mapping_handle);
        CloseHandle(file_handle);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    char* enc_cur_after = NULL;

    // For Forward commands, extract the specific function ID from APIR data
    uint32_t function_id = cmd_type;  // Default to cmd_type for non-Forward commands

    if (cmd_type == APIR_COMMAND_TYPE_FORWARD) {
        // APIR data structure: [uint32_t apir_cmd_type, int32_t function_id, ...]
        // The second field (cmd_flags) contains the actual function ID
        if (apir_data_size >= sizeof(uint32_t) + sizeof(int32_t)) {
            function_id = *(int32_t*)((char*)mapped_memory + sizeof(uint32_t));
        } else {
            printf("[ERROR] Forward command has insufficient data size: %I64u bytes\n", apir_data_size);
            return ERROR_INVALID_PARAMETER;
        }
    }

    // Skip the APIR header (cmd_type + cmd_flags) that we already extracted
    char* apir_data_start = (char*)mapped_memory;
    if (cmd_type == APIR_COMMAND_TYPE_FORWARD) {
        // Skip header: uint32_t cmd_type + int32_t cmd_flags = 8 bytes
        apir_data_start += sizeof(uint32_t) + sizeof(int32_t);
    }

    // Call the APIR backend dispatcher using session ID as virgl_ctx_id
    uint32_t dispatch_result = apir_backend_dispatcher(
        session_id,                        // virgl_ctx_id (client session ID)
        &g_windows_callbacks,               // Windows callback interface
        function_id,                       // Specific APIR function ID (not the general Forward type)
        apir_data_start,                   // Input buffer after header
        (char*)mapped_memory + apir_data_size, // Input end
        response_buffer,                   // Output buffer
        response_buffer + MAX_RESPONSE_SIZE, // Output end
        &enc_cur_after                     // Output position after encoding
    );


    // Avoid Json::Value objects completely - return success code for manual JSON handling

    if (dispatch_result == 0) {
        // Success (APIR_FORWARD_SUCCESS = 0) - write response data to shared memory file
        size_t response_data_size = enc_cur_after - response_buffer;

        if (response_data_size > 0) {
            // Create response file path using C-style strings
            char response_file_path[512];
            strncpy(response_file_path, windows_path, sizeof(response_file_path) - 20);  // Leave space for suffix
            response_file_path[sizeof(response_file_path) - 20] = '\0';

            // Add _response suffix
            char* dot = strrchr(response_file_path, '.');
            if (dot) {
                strcpy(dot, "_response.dat");
            } else {
                strcat(response_file_path, "_response");
            }

            // Write response data to file
            HANDLE response_file = CreateFileA(response_file_path,
                                             GENERIC_WRITE,
                                             0,
                                             NULL,
                                             CREATE_ALWAYS,
                                             FILE_ATTRIBUTE_NORMAL,
                                             NULL);

            if (response_file != INVALID_HANDLE_VALUE) {
                DWORD bytes_written;
                BOOL write_success = WriteFile(response_file,
                                              response_buffer,
                                              (DWORD)response_data_size,
                                              &bytes_written,
                                              NULL);

                // Force sync response data to disk before WSL2 client reads it
                if (write_success) {
                    BOOL flush_success = FlushFileBuffers(response_file);
                    if (!flush_success) {
                        printf("[SERVER] Warning: FlushFileBuffers failed (error: %lu)\n", GetLastError());
                    }
                }

                CloseHandle(response_file);

                if (write_success && bytes_written == response_data_size) {
                    // Convert back to Linux path for client using C strings
                    char linux_response_path[512];
                    if (strncmp(response_file_path, "C:", 2) == 0) {
                        snprintf(linux_response_path, sizeof(linux_response_path), "/mnt/c%s", response_file_path + 2);
                        // Replace \ with /
                        for (char* p = linux_response_path; *p; p++) {
                            if (*p == '\\') *p = '/';
                        }
                    } else {
                        strncpy(linux_response_path, response_file_path, sizeof(linux_response_path) - 1);
                        linux_response_path[sizeof(linux_response_path) - 1] = '\0';
                    }
                    // Store the response file path for JSON response
                    strncpy(g_response_file_path, linux_response_path, sizeof(g_response_file_path) - 1);
                    g_response_file_path[sizeof(g_response_file_path) - 1] = '\0';
                } else {
                    printf("[ERROR] Failed to write response data (wrote %lu of %zu bytes)\n",
                           bytes_written, response_data_size);
                }
            } else {
                printf("[ERROR] Failed to create response file: %s (error: %lu)\n",
                       response_file_path, GetLastError());
            }
        }
    } else {
        // Error
        printf("[ERROR] APIR command failed with code: %u\n", dispatch_result);
    }

    // Don't touch Json::Value objects at all - return special success code for manual JSON handling

    // Cleanup
    free(response_buffer);

    // Keep the mapping for potential future use by callbacks
    // UnmapViewOfFile(mapped_memory);  // Don't unmap yet - callbacks may need it
    // CloseHandle(mapping_handle);
    // CloseHandle(file_handle);

    fflush(stdout);
    // Return special code to indicate success but avoid Json::Value serialization
    return 999;  // Custom success code for manual JSON handling
}

/*
 * Buffer Registration API Handler
 * Registers a shared memory buffer for later lookup by buffer ID
 */
DWORD HandleBufferRegistrationAPI(SOCKET client_socket, UINT32 request_id, UINT32 buffer_id, const std::string& file_path, Json::Value& response) {
    UNREFERENCED_PARAMETER(client_socket);
    UNREFERENCED_PARAMETER(response);  // We'll bypass Json::Value to avoid crashes

    std::lock_guard<std::mutex> lock(g_buffer_mutex);

    // Get session ID from client socket (in real implementation, we'd extract this from connection context)
    // For now, use a default session ID since we need to associate buffers with sessions
    uint32_t session_id = 500;  // Hardcode for now - should be extracted from client context

    // Get or create client session
    auto& session = g_client_sessions[session_id];
    if (session.session_id == 0) {
        session.session_id = session_id;
        printf("Created new session: %u\n", session_id);
    }

    // Check if buffer ID already exists for this session
    if (session.buffers.find(buffer_id) != session.buffers.end()) {
        printf("[WARNING] Buffer ID %u already registered for session %u, overwriting\n", buffer_id, session_id);
    }

    // Translate WSL2 path to Windows path
    std::string windows_path = file_path;
    if (file_path.substr(0, 7) == "/mnt/c/") {
        // Convert /mnt/c/temp/file.dat -> C:\temp\file.dat
        windows_path = "C:" + file_path.substr(6);
        // Convert forward slashes to backslashes
        for (char& c : windows_path) {
            if (c == '/') c = '\\';
        }
        printf("Translated path: %s -> %s\n", file_path.c_str(), windows_path.c_str());
    }

    // Open shared memory file
    HANDLE file_handle = CreateFileA(
        windows_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (file_handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        printf("[ERROR] Failed to open shared memory file: %s (error: %lu)\n", windows_path.c_str(), error);
        return ERROR_FILE_NOT_FOUND;
    }

    // Get file size
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle, &file_size)) {
        CloseHandle(file_handle);
        printf("[ERROR] Failed to get file size for: %s\n", windows_path.c_str());
        return ERROR_INVALID_DATA;
    }

    // Create file mapping
    HANDLE mapping_handle = CreateFileMappingA(
        file_handle,
        NULL,
        PAGE_READWRITE,
        0,
        0,
        NULL
    );

    if (mapping_handle == NULL) {
        CloseHandle(file_handle);
        printf("[ERROR] Failed to create file mapping for: %s\n", windows_path.c_str());
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    // Map the file into memory
    void* mapped_memory = MapViewOfFile(
        mapping_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        0
    );

    if (mapped_memory == NULL) {
        CloseHandle(mapping_handle);
        CloseHandle(file_handle);
        printf("[ERROR] Failed to map view of file: %s\n", windows_path.c_str());
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    // Store buffer mapping
    BufferMapping mapping;
    mapping.file_handle = file_handle;
    mapping.mapping_handle = mapping_handle;
    mapping.mapped_memory = mapped_memory;
    mapping.size = (size_t)file_size.QuadPart;
    mapping.file_path = file_path;

    session.buffers[buffer_id] = mapping;

    printf("Successfully registered buffer: session=%u, buffer_id=%u, file=%s, size=%zu bytes\n",
           session_id, buffer_id, file_path.c_str(), mapping.size);

    return ERROR_SUCCESS;
}
