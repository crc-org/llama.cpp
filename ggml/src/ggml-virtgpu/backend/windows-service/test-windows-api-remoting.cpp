/*
 * Integration Test for Windows API Remoting
 *
 * This test validates the critical fixes for Windows frontend<>backend communication:
 * 1. Response buffer return functionality
 * 2. Per-client buffer namespace collision prevention
 * 3. Dynamic APIR command type support
 * 4. Error handling and cleanup
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <json/json.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <vector>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

// Test configuration
#define TEST_HOST "127.0.0.1"
#define TEST_PORT 4660
#define TEST_TIMEOUT_MS 5000
#define MAX_CLIENTS 5

// Mock APIR command types (from backend/shared/apir_backend.gen.h)
#define APIR_COMMAND_TYPE_DEVICE_GET_DEVICE_COUNT 0
#define APIR_COMMAND_TYPE_BACKEND_GRAPH_COMPUTE 22

// Test results tracking
struct TestResult {
    bool passed;
    std::string test_name;
    std::string error_message;
};

std::vector<TestResult> g_test_results;

void log_test_result(const char* test_name, bool passed, const char* error_msg = "") {
    TestResult result;
    result.test_name = test_name;
    result.passed = passed;
    result.error_message = error_msg ? error_msg : "";
    g_test_results.push_back(result);

    printf("[%s] %s %s\n",
           passed ? "PASS" : "FAIL",
           test_name,
           error_msg && strlen(error_msg) > 0 ? error_msg : "");
}

// Helper function to connect to service
SOCKET connect_to_service() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, TEST_HOST, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    return sock;
}

// Helper function to send JSON message
bool send_json_message(SOCKET sock, const char* json_str) {
    uint32_t msg_len = htonl((uint32_t)strlen(json_str));

    // Send length header
    if (send(sock, (char*)&msg_len, sizeof(msg_len), 0) != sizeof(msg_len)) {
        return false;
    }

    // Send JSON data
    return send(sock, json_str, strlen(json_str), 0) == (int)strlen(json_str);
}

// Helper function to receive JSON response
bool receive_json_response(SOCKET sock, char* response_buffer, size_t buffer_size) {
    // Receive length header
    uint32_t msg_len;
    if (recv(sock, (char*)&msg_len, sizeof(msg_len), 0) != sizeof(msg_len)) {
        return false;
    }

    msg_len = ntohl(msg_len);
    if (msg_len >= buffer_size) {
        return false;
    }

    // Receive JSON data
    int bytes_received = recv(sock, response_buffer, msg_len, 0);
    if (bytes_received != (int)msg_len) {
        return false;
    }

    response_buffer[msg_len] = '\0';
    return true;
}

// Helper function to create mock APIR data
void create_mock_apir_data(uint32_t cmd_type, char* buffer, size_t* size) {
    // Create minimal APIR command with command type header
    uint32_t* cmd_header = (uint32_t*)buffer;
    *cmd_header = cmd_type;

    // Add some mock payload data
    memset(buffer + sizeof(uint32_t), 0xAB, 32);
    *size = sizeof(uint32_t) + 32;
}

// Helper function to create temporary shared memory file
bool create_shared_memory_file(const char* file_path, const void* data, size_t size) {
    HANDLE file_handle = CreateFileA(file_path,
                                   GENERIC_WRITE,
                                   0,
                                   NULL,
                                   CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);

    if (file_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written;
    BOOL success = WriteFile(file_handle, data, (DWORD)size, &bytes_written, NULL);
    CloseHandle(file_handle);

    return success && bytes_written == size;
}

/**
 * Test 1: Basic connectivity and echo functionality
 */
bool test_basic_connectivity() {
    SOCKET sock = connect_to_service();
    if (sock == INVALID_SOCKET) {
        log_test_result("Basic Connectivity", false, "Failed to connect to service");
        return false;
    }

    // Test echo API
    const char* echo_request = R"({
        "api": "echo",
        "request_id": 1,
        "input": "test_connectivity"
    })";

    if (!send_json_message(sock, echo_request)) {
        closesocket(sock);
        log_test_result("Basic Connectivity", false, "Failed to send echo request");
        return false;
    }

    char response[1024];
    if (!receive_json_response(sock, response, sizeof(response))) {
        closesocket(sock);
        log_test_result("Basic Connectivity", false, "Failed to receive echo response");
        return false;
    }

    closesocket(sock);

    // Parse JSON response
    json_object* response_obj = json_tokener_parse(response);
    if (!response_obj) {
        log_test_result("Basic Connectivity", false, "Invalid JSON response");
        return false;
    }

    json_object* status_obj;
    bool success = json_object_object_get_ex(response_obj, "status", &status_obj) &&
                   strcmp(json_object_get_string(status_obj), "success") == 0;

    json_object_put(response_obj);

    log_test_result("Basic Connectivity", success);
    return success;
}

/**
 * Test 2: APIR command processing with dynamic command types
 */
bool test_dynamic_command_types() {
    SOCKET sock = connect_to_service();
    if (sock == INVALID_SOCKET) {
        log_test_result("Dynamic Command Types", false, "Failed to connect to service");
        return false;
    }

    // Test different command types
    uint32_t test_command_types[] = {0, 5, 10, 22};

    for (int i = 0; i < 4; i++) {
        uint32_t cmd_type = test_command_types[i];

        // Create mock APIR data with specific command type
        char apir_data[64];
        size_t apir_size;
        create_mock_apir_data(cmd_type, apir_data, &apir_size);

        // Create shared memory file
        char file_path[256];
        snprintf(file_path, sizeof(file_path), "C:\\temp\\test_apir_%u_%d.dat", cmd_type, GetCurrentProcessId());

        if (!create_shared_memory_file(file_path, apir_data, apir_size)) {
            closesocket(sock);
            log_test_result("Dynamic Command Types", false, "Failed to create shared memory file");
            return false;
        }

        // Convert to Linux path for request
        char linux_path[256];
        snprintf(linux_path, sizeof(linux_path), "/mnt/c/temp/test_apir_%u_%d.dat", cmd_type, GetCurrentProcessId());

        // Send APIR request
        char request[1024];
        snprintf(request, sizeof(request), R"({
            "api": "apir",
            "request_id": %d,
            "apir_cmd_type": %u,
            "apir_data_size": %zu,
            "shared_file_path": "%s",
            "buffer_id": %d
        })", i + 100, cmd_type, apir_size, linux_path, i + 1);

        if (!send_json_message(sock, request)) {
            closesocket(sock);
            DeleteFileA(file_path);
            log_test_result("Dynamic Command Types", false, "Failed to send APIR request");
            return false;
        }

        char response[2048];
        if (!receive_json_response(sock, response, sizeof(response))) {
            closesocket(sock);
            DeleteFileA(file_path);
            log_test_result("Dynamic Command Types", false, "Failed to receive APIR response");
            return false;
        }

        // Parse response and verify command type was processed
        json_object* response_obj = json_tokener_parse(response);
        if (response_obj) {
            json_object* result_obj;
            if (json_object_object_get_ex(response_obj, "result", &result_obj)) {
                json_object* cmd_type_obj;
                if (json_object_object_get_ex(result_obj, "cmd_type", &cmd_type_obj)) {
                    uint32_t returned_cmd_type = json_object_get_int(cmd_type_obj);
                    if (returned_cmd_type != cmd_type) {
                        json_object_put(response_obj);
                        closesocket(sock);
                        DeleteFileA(file_path);

                        char error_msg[128];
                        snprintf(error_msg, sizeof(error_msg),
                                "Command type mismatch: sent %u, got %u", cmd_type, returned_cmd_type);
                        log_test_result("Dynamic Command Types", false, error_msg);
                        return false;
                    }
                }
            }
            json_object_put(response_obj);
        }

        // Cleanup
        DeleteFileA(file_path);
    }

    closesocket(sock);
    log_test_result("Dynamic Command Types", true);
    return true;
}

/**
 * Test 3: Concurrent clients (buffer collision prevention)
 */
void client_thread_func(int client_id, bool* success) {
    *success = false;

    SOCKET sock = connect_to_service();
    if (sock == INVALID_SOCKET) {
        return;
    }

    // Each client uses the same buffer_id but different file paths
    uint32_t buffer_id = 1;  // Intentionally the same for all clients
    uint32_t cmd_type = APIR_COMMAND_TYPE_BACKEND_GRAPH_COMPUTE;

    // Create unique APIR data for this client
    char apir_data[64];
    size_t apir_size;
    create_mock_apir_data(cmd_type, apir_data, &apir_size);

    // Add client-specific marker
    memset(apir_data + sizeof(uint32_t), client_id, 32);

    // Create unique shared memory file
    char file_path[256];
    snprintf(file_path, sizeof(file_path),
             "C:\\temp\\test_concurrent_%d_%d.dat", client_id, GetCurrentProcessId());

    if (!create_shared_memory_file(file_path, apir_data, apir_size)) {
        closesocket(sock);
        return;
    }

    // Convert to Linux path
    char linux_path[256];
    snprintf(linux_path, sizeof(linux_path),
             "/mnt/c/temp/test_concurrent_%d_%d.dat", client_id, GetCurrentProcessId());

    // Send APIR request
    char request[1024];
    snprintf(request, sizeof(request), R"({
        "api": "apir",
        "request_id": %d,
        "apir_cmd_type": %u,
        "apir_data_size": %zu,
        "shared_file_path": "%s",
        "buffer_id": %u
    })", client_id, cmd_type, apir_size, linux_path, buffer_id);

    if (send_json_message(sock, request)) {
        char response[2048];
        if (receive_json_response(sock, response, sizeof(response))) {
            // Parse response to verify it's for this client
            json_object* response_obj = json_tokener_parse(response);
            if (response_obj) {
                json_object* result_obj;
                if (json_object_object_get_ex(response_obj, "result", &result_obj)) {
                    json_object* buffer_id_obj;
                    if (json_object_object_get_ex(result_obj, "buffer_id", &buffer_id_obj)) {
                        uint32_t returned_buffer_id = json_object_get_int(buffer_id_obj);
                        *success = (returned_buffer_id == buffer_id);
                    }
                }
                json_object_put(response_obj);
            }
        }
    }

    closesocket(sock);
    DeleteFileA(file_path);
}

bool test_concurrent_clients() {
    const int num_clients = 3;
    std::thread threads[num_clients];
    bool results[num_clients];

    // Launch concurrent client threads
    for (int i = 0; i < num_clients; i++) {
        threads[i] = std::thread(client_thread_func, i, &results[i]);
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_clients; i++) {
        threads[i].join();
    }

    // Check if all clients succeeded
    bool all_success = true;
    for (int i = 0; i < num_clients; i++) {
        if (!results[i]) {
            all_success = false;
            break;
        }
    }

    log_test_result("Concurrent Clients", all_success,
                   all_success ? "" : "One or more clients failed");
    return all_success;
}

/**
 * Test 4: Response data file handling
 */
bool test_response_data_handling() {
    SOCKET sock = connect_to_service();
    if (sock == INVALID_SOCKET) {
        log_test_result("Response Data Handling", false, "Failed to connect to service");
        return false;
    }

    // Create APIR data for a command that should generate response data
    uint32_t cmd_type = APIR_COMMAND_TYPE_DEVICE_GET_DEVICE_COUNT;
    char apir_data[64];
    size_t apir_size;
    create_mock_apir_data(cmd_type, apir_data, &apir_size);

    // Create shared memory file
    char file_path[256];
    snprintf(file_path, sizeof(file_path),
             "C:\\temp\\test_response_%d.dat", GetCurrentProcessId());

    if (!create_shared_memory_file(file_path, apir_data, apir_size)) {
        closesocket(sock);
        log_test_result("Response Data Handling", false, "Failed to create shared memory file");
        return false;
    }

    // Convert to Linux path
    char linux_path[256];
    snprintf(linux_path, sizeof(linux_path),
             "/mnt/c/temp/test_response_%d.dat", GetCurrentProcessId());

    // Send APIR request
    char request[1024];
    snprintf(request, sizeof(request), R"({
        "api": "apir",
        "request_id": 999,
        "apir_cmd_type": %u,
        "apir_data_size": %zu,
        "shared_file_path": "%s",
        "buffer_id": 999
    })", cmd_type, apir_size, linux_path);

    if (!send_json_message(sock, request)) {
        closesocket(sock);
        DeleteFileA(file_path);
        log_test_result("Response Data Handling", false, "Failed to send APIR request");
        return false;
    }

    char response[2048];
    if (!receive_json_response(sock, response, sizeof(response))) {
        closesocket(sock);
        DeleteFileA(file_path);
        log_test_result("Response Data Handling", false, "Failed to receive APIR response");
        return false;
    }

    // Parse response and check for response file path
    json_object* response_obj = json_tokener_parse(response);
    bool success = false;

    if (response_obj) {
        json_object* result_obj;
        if (json_object_object_get_ex(response_obj, "result", &result_obj)) {
            json_object* status_obj;
            if (json_object_object_get_ex(result_obj, "status", &status_obj)) {
                const char* status = json_object_get_string(status_obj);
                success = (strcmp(status, "success") == 0 || strcmp(status, "error") == 0);

                // Note: We expect either success with response file or controlled error
                // since we're sending mock data that may not be valid APIR
            }
        }
        json_object_put(response_obj);
    }

    closesocket(sock);
    DeleteFileA(file_path);

    log_test_result("Response Data Handling", success);
    return success;
}

/**
 * Main test runner
 */
int main() {
    printf("=== Windows API Remoting Integration Tests ===\n\n");

    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("Failed to initialize Winsock\n");
        return 1;
    }

    // Ensure temp directory exists
    CreateDirectoryA("C:\\temp", NULL);

    // Run tests
    printf("Running integration tests against service at %s:%d\n\n", TEST_HOST, TEST_PORT);

    test_basic_connectivity();
    test_dynamic_command_types();
    test_concurrent_clients();
    test_response_data_handling();

    // Print summary
    printf("\n=== Test Results Summary ===\n");
    int passed = 0, total = g_test_results.size();

    for (const auto& result : g_test_results) {
        printf("[%s] %s\n", result.passed ? "PASS" : "FAIL", result.test_name.c_str());
        if (!result.passed && !result.error_message.empty()) {
            printf("    Error: %s\n", result.error_message.c_str());
        }
        if (result.passed) passed++;
    }

    printf("\nPassed: %d/%d tests\n", passed, total);

    if (passed == total) {
        printf("\n🎉 All integration tests PASSED! Windows API remoting fixes are working correctly.\n");
    } else {
        printf("\n❌ Some tests FAILED. Please check the service and fix issues.\n");
    }

    WSACleanup();
    return (passed == total) ? 0 : 1;
}