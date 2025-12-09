#include "ggml-remoting.h"

// Static member definitions
std::unordered_map<apir_buffer_host_handle_t, ggml_backend_buffer_t> ggml_remoting_buffer_tracker::backend_to_frontend_map;
std::mutex ggml_remoting_buffer_tracker::map_mutex;

// Static methods implementation
void ggml_remoting_buffer_tracker::register_buffer(ggml_backend_buffer_t frontend_buffer, apir_buffer_host_handle_t backend_handle) {
    std::lock_guard<std::mutex> lock(map_mutex);
    backend_to_frontend_map[backend_handle] = frontend_buffer;
}

void ggml_remoting_buffer_tracker::unregister_buffer(apir_buffer_host_handle_t backend_handle) {
    std::lock_guard<std::mutex> lock(map_mutex);
    auto it = backend_to_frontend_map.find(backend_handle);
    if (it != backend_to_frontend_map.end()) {
        backend_to_frontend_map.erase(it);
    } else {
        WARNING("Attempted to unregister unknown backend handle 0x%lx", backend_handle);
    }
}

ggml_backend_buffer_t ggml_remoting_buffer_tracker::lookup_frontend_buffer(apir_buffer_host_handle_t backend_handle) {
    std::lock_guard<std::mutex> lock(map_mutex);
    auto it = backend_to_frontend_map.find(backend_handle);
    if (it != backend_to_frontend_map.end()) {
        return it->second;
    }
    return nullptr;
}

bool ggml_remoting_buffer_tracker::is_registered(apir_buffer_host_handle_t backend_handle) {
    std::lock_guard<std::mutex> lock(map_mutex);
    return backend_to_frontend_map.find(backend_handle) != backend_to_frontend_map.end();
}

// Convenience functions implementation
void ggml_remoting_register_buffer(ggml_backend_buffer_t frontend_buffer) {
    if (!frontend_buffer) {
        ERROR("Cannot register null frontend buffer");
        return;
    }

    apir_buffer_host_handle_t backend_handle = ggml_buffer_to_apir_handle(frontend_buffer);
    ggml_remoting_buffer_tracker::register_buffer(frontend_buffer, backend_handle);
}

void ggml_remoting_unregister_buffer(ggml_backend_buffer_t frontend_buffer) {
    if (!frontend_buffer) {
        ERROR("Cannot unregister null frontend buffer");
        return;
    }

    apir_buffer_host_handle_t backend_handle = ggml_buffer_to_apir_handle(frontend_buffer);
    ggml_remoting_buffer_tracker::unregister_buffer(backend_handle);
}

ggml_backend_buffer_t ggml_remoting_lookup_frontend_buffer_from_backend_handle(apir_buffer_host_handle_t backend_handle) {
    return ggml_remoting_buffer_tracker::lookup_frontend_buffer(backend_handle);
}

// Additional utility methods
size_t ggml_remoting_buffer_tracker::get_buffer_count() {
    std::lock_guard<std::mutex> lock(map_mutex);
    return backend_to_frontend_map.size();
}

void ggml_remoting_buffer_tracker::print_buffer_info() {
    std::lock_guard<std::mutex> lock(map_mutex);
    printf("INFO: Buffer Tracker - %zu buffers tracked:\n", backend_to_frontend_map.size());
    for (const auto& pair : backend_to_frontend_map) {
        printf("  Backend handle: 0x%lx -> Frontend buffer: 0x%p\n",
               pair.first, (void *) pair.second);
    }
}
