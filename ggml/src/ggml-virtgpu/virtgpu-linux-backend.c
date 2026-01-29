/*
 * Linux VirtGPU Backend Adapter
 *
 * This file provides an adapter between the restored original Linux VirtGPU
 * implementation (virtgpu.cpp) and the new backend interface architecture.
 */

#include "./virtgpu-interface.h"
#include "virtgpu.h"
#include "virtgpu-shm.h"
#include "backend/shared/api_remoting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Simple logging macros */
#define GGML_LOG_INFO(fmt, ...)  printf("GGML-INFO: " fmt, ##__VA_ARGS__)
#define GGML_LOG_ERROR(fmt, ...) fprintf(stderr, "GGML-ERROR: " fmt, ##__VA_ARGS__)
#define GGML_LOG_DEBUG(fmt, ...) printf("GGML-DEBUG: " fmt, ##__VA_ARGS__)

/* Forward declaration */
static const virtgpu_backend_ops linux_ops;

/* Linux backend adapter implementations */
static virtgpu* linux_create(void) {
    GGML_LOG_INFO("Linux DRM VirtGPU backend: calling original create_virtgpu()\n");

    // Create the original virtgpu structure
    struct virtgpu* original_gpu = create_virtgpu();
    if (!original_gpu) {
        GGML_LOG_ERROR("Failed to create original Linux virtgpu\n");
        return NULL;
    }

    // Create the interface virtgpu structure
    virtgpu* interface_gpu = (virtgpu*)malloc(sizeof(virtgpu));
    if (!interface_gpu) {
        GGML_LOG_ERROR("Failed to allocate interface virtgpu structure\n");
        // TODO: Add proper cleanup for original_gpu
        return NULL;
    }

    // Initialize the interface structure
    memset(interface_gpu, 0, sizeof(virtgpu));
    interface_gpu->use_apir_capset = original_gpu->use_apir_capset;
    interface_gpu->backend_type = VIRTGPU_BACKEND_LINUX_DRM;
    interface_gpu->ops = NULL; // Will be set by caller

    // Copy shared memory structures
    interface_gpu->reply_shmem = original_gpu->reply_shmem;
    interface_gpu->data_shmem = original_gpu->data_shmem;
    interface_gpu->shmem_array = original_gpu->shmem_array;

    // Store the original structure in backend_data
    interface_gpu->backend_data = original_gpu;

    return interface_gpu;
}

static void linux_destroy(virtgpu* gpu) {
    GGML_LOG_INFO("Linux DRM VirtGPU backend: destroying gpu instance\n");
    if (gpu && gpu->backend_data) {
        // Get the original virtgpu structure
        struct virtgpu* original_gpu = (struct virtgpu*)gpu->backend_data;

        // TODO: Add proper cleanup for the original virtgpu structure
        // The original implementation doesn't have a cleanup function
        GGML_LOG_INFO("Linux backend: cleanup would need to be implemented\n");

        // Free the interface structure
        free(gpu);
    }
}

static apir_encoder* linux_remote_call_prepare(virtgpu* gpu, int apir_cmd_type, int32_t cmd_flags) {
    if (!gpu || !gpu->backend_data) {
        GGML_LOG_ERROR("Invalid virtgpu handle in remote_call_prepare\n");
        return NULL;
    }

    // Get the original virtgpu structure
    struct virtgpu* original_gpu = (struct virtgpu*)gpu->backend_data;

    // Call the original function
    return remote_call_prepare(original_gpu, apir_cmd_type, cmd_flags);
}

static uint32_t linux_remote_call(virtgpu* gpu, apir_encoder* enc, apir_decoder** dec, uint64_t timeout_ms, long long* call_duration_ns) {
    if (!gpu || !gpu->backend_data || !enc || !dec) {
        GGML_LOG_ERROR("Invalid parameters in remote_call\n");
        return 1; // Error code
    }

    // Get the original virtgpu structure
    struct virtgpu* original_gpu = (struct virtgpu*)gpu->backend_data;

    // Convert timeout from uint64_t milliseconds to float milliseconds
    float max_wait_ms = (float)timeout_ms;

    // Call the original function
    return remote_call(original_gpu, enc, dec, max_wait_ms, call_duration_ns);
}

static void linux_remote_call_finish(virtgpu* gpu, apir_encoder* enc, apir_decoder* dec) {
    if (!gpu || !gpu->backend_data) {
        GGML_LOG_ERROR("Invalid virtgpu handle in remote_call_finish\n");
        return;
    }

    // Get the original virtgpu structure
    struct virtgpu* original_gpu = (struct virtgpu*)gpu->backend_data;

    // Call the original function
    remote_call_finish(original_gpu, enc, dec);
}

static int linux_shmem_create(virtgpu* gpu, size_t size, virtgpu_shmem* shmem) {
    if (!gpu || !gpu->backend_data || !shmem) {
        GGML_LOG_ERROR("Invalid parameters in shmem_create\n");
        return -1;
    }

    // Get the original virtgpu structure
    struct virtgpu* original_gpu = (struct virtgpu*)gpu->backend_data;

    // Call the original virtgpu_shmem_create function
    return virtgpu_shmem_create(original_gpu, size, shmem);
}

static void linux_shmem_destroy(virtgpu* gpu, virtgpu_shmem* shmem) {
    if (!gpu || !gpu->backend_data || !shmem) {
        GGML_LOG_ERROR("Invalid parameters in shmem_destroy\n");
        return;
    }

    // Get the original virtgpu structure
    struct virtgpu* original_gpu = (struct virtgpu*)gpu->backend_data;

    // Call the original virtgpu_shmem_destroy function
    virtgpu_shmem_destroy(original_gpu, shmem);
}

static void* linux_shmem_get_ptr(virtgpu_shmem* shmem) {
    if (!shmem) {
        return NULL;
    }

    // Return the mmap_ptr from the original virtgpu_shmem structure
    return shmem->mmap_ptr;
}

/* Linux backend operations table */
static const virtgpu_backend_ops linux_ops = {
    .name = "Linux DRM VirtGPU (Original)",
    .create = linux_create,
    .destroy = linux_destroy,
    .remote_call_prepare = linux_remote_call_prepare,
    .remote_call = linux_remote_call,
    .remote_call_finish = linux_remote_call_finish,
    .shmem_create = linux_shmem_create,
    .shmem_destroy = linux_shmem_destroy,
    .shmem_get_ptr = linux_shmem_get_ptr,
    .sparse_array_init = util_sparse_array_init,
    .sparse_array_finish = util_sparse_array_finish,
    .sparse_array_get = util_sparse_array_get,
    .sparse_array_set = util_sparse_array_set,
};

/* Public interface */
const virtgpu_backend_ops* virtgpu_backend_linux_drm_get_ops(void) {
    return &linux_ops;
}

/*
 * NOTE: This adapter bridges the original Linux DRM VirtGPU implementation
 * with the new backend interface architecture. It allows the restored
 * virtgpu.cpp to work alongside winApiRmt.c through a common interface.
 *
 * The original implementation is fully functional and complete, so this
 * adapter just provides the necessary function signature translations.
 */