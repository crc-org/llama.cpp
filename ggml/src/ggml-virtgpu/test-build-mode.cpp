/*
 * Test program to verify which build mode is active
 *
 * This program can be compiled to test whether the Windows or Linux
 * backend is being used, and verify that the build system is working correctly.
 */

#include "virtgpu.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("ggml-virtgpu Build Mode Test\n");
    printf("============================\n");

#ifdef GGML_VIRTGPU_USE_WINDOWS
    printf("Backend: Windows winApiRmt\n");
    printf("Transport: Hyper-V socket + TCP fallback\n");
    printf("Shared memory: File-backed (/mnt/c/)\n");
    printf("Protocol: APIR over winApiRmt\n");
    printf("\n");

    /* Test basic Windows backend initialization */
    printf("Testing Windows backend initialization...\n");

    virtgpu* gpu = create_virtgpu();
    if (gpu) {
        printf("SUCCESS: Windows virtgpu created successfully\n");
        printf("winApiRmt handle: %p\n", gpu->winapi_handle);

        /* Test basic functionality */
        printf("\nTesting shared memory allocation...\n");
        virtgpu_shmem test_shmem;
        if (virtgpu_shmem_create(gpu, &test_shmem, 4096) == 0) {
            printf("SUCCESS: Allocated 4KB shared buffer\n");
            printf("Buffer address: %p\n", virtgpu_shmem_get_ptr(&test_shmem));

            virtgpu_shmem_destroy(gpu, &test_shmem);
            printf("SUCCESS: Cleaned up shared buffer\n");
        } else {
            printf("ERROR: Failed to allocate shared buffer\n");
        }

        /* Note: Can't test full remoting without Windows service */
        printf("\nNote: Full remoting test requires winApiRmt Windows service\n");

        /* Cleanup - note: create_virtgpu doesn't have a destroy function yet */
        printf("\nWindows backend test complete\n");
    } else {
        printf("ERROR: Failed to create Windows virtgpu\n");
        printf("Possible causes:\n");
        printf("1. winApiRmt Windows service not running\n");
        printf("2. No network connectivity to Windows host\n");
        printf("3. Shared memory path not accessible\n");
        return 1;
    }

#else
    printf("Backend: Linux DRM\n");
    printf("Transport: VirtIO GPU DRM ioctls\n");
    printf("Shared memory: DRM GEM buffers\n");
    printf("Protocol: APIR over virtgpu hypervisor\n");
    printf("\n");

    /* Test basic Linux backend initialization */
    printf("Testing Linux backend initialization...\n");

    virtgpu* gpu = create_virtgpu();
    if (gpu) {
        printf("SUCCESS: Linux virtgpu created successfully\n");
        printf("DRM fd: %d\n", gpu->fd);

        /* Note: Can't test full DRM functionality without virtgpu driver */
        printf("\nNote: Full DRM test requires virtgpu driver and hypervisor\n");

        printf("\nLinux backend test complete\n");
    } else {
        printf("ERROR: Failed to create Linux virtgpu\n");
        printf("Possible causes:\n");
        printf("1. virtgpu DRM driver not loaded\n");
        printf("2. No virtgpu device available\n");
        printf("3. Insufficient permissions\n");
        return 1;
    }
#endif

    printf("\n============================\n");
    printf("Build configuration test passed!\n");
    return 0;
}