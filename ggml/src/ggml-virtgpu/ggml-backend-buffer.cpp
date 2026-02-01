#include "ggml-remoting.h"
#include "virtgpu-shm.h"
#include <pthread.h>

#define BUFFER_TO_GPU(name) ((ggml_backend_remoting_buffer_context *) (name)->context)->gpu

// Simple checksum for data verification
static uint32_t simple_checksum(const void * data, size_t size) {
    const uint8_t * bytes = (const uint8_t *)data;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum * 31) + bytes[i];
    }
    return checksum;
}

#define GUEST_CHECKSUM 1
#define VERIFY_SET_TENSOR_CACHE_COHERENCY 1

#if GUEST_CHECKSUM == 1
// Static IDs for matching guest/host operations
#if VERIFY_SET_TENSOR_CACHE_COHERENCY == 1
static uint32_t buffer_set_tensor_id = 0;
#endif
static uint32_t buffer_get_tensor_id = 0;
#endif

static void * ggml_backend_remoting_buffer_get_base(ggml_backend_buffer_t buffer) {
    ggml_backend_remoting_buffer_context * context = (ggml_backend_remoting_buffer_context *) buffer->context;
    if (context->base) {
        return context->base;
    }

    context->base = apir_buffer_get_base(BUFFER_TO_GPU(buffer), BUFFER_TO_APIR_CONTEXT(buffer));

    return context->base;
}

static void ggml_backend_remoting_buffer_set_tensor(ggml_backend_buffer_t buffer,
                                                    ggml_tensor *         tensor,
                                                    const void *          data,
                                                    size_t                offset,
                                                    size_t                size) {
    ggml_backend_remoting_buffer_context * context = BUFFER_TO_GGML_CONTEXT(buffer);

    if (context->is_from_ptr) {
        // Simple operation: just write data to shared buffer
        memcpy((char *) tensor->data + offset, data, size);

        // Simple diagnostic to confirm set_tensor is being called
        static uint32_t set_tensor_count = 0;
        set_tensor_count++;
        if (set_tensor_count <= 3) {
            uint32_t checksum = simple_checksum(data, size);
            printf("[SET_DIAG] set_tensor #%u: writing checksum=0x%08x size=%zu to tensor=%p+%zu\n",
                   set_tensor_count, checksum, size, tensor->data, offset);
        }

#if VERIFY_SET_TENSOR_CACHE_COHERENCY == 1
        virtgpu * gpu = BUFFER_TO_GPU(buffer);

        // Assign static ID for matching
        uint32_t operation_id = ++buffer_set_tensor_id;

        // 2. Show checksum of the data written
        uint32_t checksum = simple_checksum(data, size);

        // 3. Get buffer context and use res_id for matching
        apir_buffer_context_t * buffer_ctx = BUFFER_TO_APIR_CONTEXT(buffer);
        uint32_t res_id = buffer_ctx->shmem.res_id;

        // DEBUG: Show tensor->data vs shared buffer locations
        printf("DEBUG: tensor->data=%p, mmap_ptr=%p, offset=%zu\n",
               tensor->data, buffer_ctx->shmem.mmap_ptr, offset);

        printf("GUEST #%u res_id=%u set_tensor: offset=%zu size=%zu checksum=0x%08x\n",
               operation_id, res_id, offset, size, checksum);

        // Calculate INPUT data checksum for validation
        uint32_t input_checksum = simple_checksum(data, size);

        // Calculate buffer data checksum after write
        uint32_t buffer_checksum = simple_checksum((const char *) tensor->data + offset, size);

        // Calculate file offset from beginning
        size_t tensor_offset_from_base = (char*)tensor->data - (char*)buffer_ctx->shmem.mmap_ptr;
        size_t file_offset = tensor_offset_from_base + offset;

        printf("GUEST #%u res_id=%u VALIDATION: input=0x%08x buffer=0x%08x\n",
               operation_id, res_id, input_checksum, buffer_checksum);
        printf("GUEST #%u res_id=%u ADDRESSES: tensor->data=%p mmap_ptr=%p tensor_offset=0x%zx file_offset=0x%zx\n",
               operation_id, res_id, tensor->data, buffer_ctx->shmem.mmap_ptr, tensor_offset_from_base, file_offset);

        // VALIDATE: tensor->data must be within shared buffer bounds
        void* buffer_start = buffer_ctx->shmem.mmap_ptr;
        void* buffer_end = (char*)buffer_ctx->shmem.mmap_ptr + buffer_ctx->shmem.mmap_size;
        bool tensor_within_buffer = (tensor->data >= buffer_start && tensor->data < buffer_end);

        printf("GUEST #%u res_id=%u BOUNDS_CHECK: buffer=[%p-%p] size=0x%zx tensor_within_buffer=%s\n",
               operation_id, res_id, buffer_start, buffer_end, buffer_ctx->shmem.mmap_size,
               tensor_within_buffer ? "TRUE" : "FALSE");

        // 4. CACHE COHERENCY: Unmap tensor buffer so host can see our writes
        void * original_mmap_ptr = buffer_ctx->shmem.mmap_ptr;  // Store base mapping address
        void * original_tensor_data = tensor->data;              // Store tensor offset for verification
        virtgpu_shmem_unmap_for_host(&buffer_ctx->shmem);

        // 5. Trigger remote call for host verification - use absolute file offset
        apir_buffer_set_tensor(gpu, buffer_ctx, tensor, data, file_offset, size);

        // 6. CACHE COHERENCY: Remap tensor buffer to see host changes
        virtgpu_shmem_remap_after_host(&buffer_ctx->shmem, original_mmap_ptr);

        // 7. Verify tensor->data pointer is still valid after remapping
        if (tensor->data != original_tensor_data) {
            printf("FATAL: Tensor data pointer changed after remapping: %p -> %p\n",
                   original_tensor_data, tensor->data);
            exit(1);
        }
#endif
    } else {
        printf("set_tensor not from ptr\n");
        exit(1);
    }

    return;
}

static void ggml_backend_remoting_buffer_get_tensor(ggml_backend_buffer_t buffer,
                                                    const ggml_tensor *   tensor,
                                                    void *                data,
                                                    size_t                offset,
                                                    size_t                size) {
#if GUEST_CHECKSUM == 1
    virtgpu *                              gpu     = BUFFER_TO_GPU(buffer);
#endif
    ggml_backend_remoting_buffer_context * context = BUFFER_TO_GGML_CONTEXT(buffer);

    if (context->is_from_ptr) {

#if GUEST_CHECKSUM == 1
        // Assign static ID for matching
        uint32_t operation_id = ++buffer_get_tensor_id;

        // Get buffer context and use res_id for matching
        apir_buffer_context_t * buffer_ctx = BUFFER_TO_APIR_CONTEXT(buffer);
        uint32_t res_id = buffer_ctx->shmem.res_id;

        // CACHE COHERENCY: Unmap tensor buffer so host can write to it
        void * original_mmap_ptr = buffer_ctx->shmem.mmap_ptr;  // Store base mapping address
        void * original_tensor_data = (void*)tensor->data;       // Store tensor offset for verification
        virtgpu_shmem_unmap_for_host(&buffer_ctx->shmem);

        // CACHE COHERENCY: Remap tensor buffer to see host changes
        virtgpu_shmem_remap_after_host(&buffer_ctx->shmem, original_mmap_ptr);

        // Verify tensor->data pointer is still valid after remapping
        if (tensor->data != original_tensor_data) {
            printf("FATAL: Tensor data pointer changed after remapping: %p -> %p\n",
                   original_tensor_data, tensor->data);
            exit(1);
        }

        // Calculate file offset from beginning (same as set_tensor)
        size_t tensor_offset_from_base = (char*)tensor->data - (char*)buffer_ctx->shmem.mmap_ptr;
        size_t file_offset = tensor_offset_from_base + offset;

        // Calculate checksum of buffer data for host comparison
        uint32_t guest_checksum = simple_checksum((const char *) tensor->data + offset, size);

        // Trigger remote call for host to verify data (using absolute file offset)
        apir_buffer_get_tensor(gpu, buffer_ctx, tensor, data, file_offset, size, guest_checksum);
#endif

        // 1. Call the memcpy (normal operation)
        memcpy(data, (const char *) tensor->data + offset, size);

        // Simple checksum diagnostic (independent of GUEST_CHECKSUM)
        uint32_t buffer_checksum = simple_checksum((const char *) tensor->data + offset, size);
        uint32_t data_checksum = simple_checksum(data, size);

        static uint32_t get_tensor_count = 0;
        get_tensor_count++;

        if (get_tensor_count <= 5) {  // Only show first few for debugging
            printf("[GET_DIAG] get_tensor #%u: buffer_checksum=0x%08x data_checksum=0x%08x size=%zu\n",
                   get_tensor_count, buffer_checksum, data_checksum, size);
        }
#if GUEST_CHECKSUM == 1
        // 2. Show checksum of the data read (should match buffer data)
        printf("GUEST #%u res_id=%u get_tensor: offset=%zu size=%zu buffer_checksum=0x%08x data_checksum=0x%08x\n",
               operation_id, res_id, offset, size, guest_checksum, data_checksum);
#endif

    } else {
        printf("get_tensor not from ptr\n");
        exit(1);
    }
}

static void ggml_backend_remoting_buffer_set_tensor_from_ptr(ggml_backend_buffer_t buffer,
                                                             ggml_tensor *         tensor,
                                                             const void *          data,
                                                             size_t                offset,
                                                             size_t                size) {
    UNUSED(buffer);
    printf("%s\n", __func__);
    exit(1);
    memcpy((char *) tensor->data + offset, data, size);

    return;
}

static void ggml_backend_remoting_buffer_get_tensor_from_ptr(ggml_backend_buffer_t buffer,
                                                             const ggml_tensor *   tensor,
                                                             void *                data,
                                                             size_t                offset,
                                                             size_t                size) {
    UNUSED(buffer);
    printf("%s\n", __func__);
    exit(1);
    memcpy(data, (const char *) tensor->data + offset, size);
}

static bool ggml_backend_remoting_buffer_cpy_tensor(ggml_backend_buffer_t buffer,
                                                    const ggml_tensor *   src,
                                                    ggml_tensor *         dst) {
    virtgpu * gpu = BUFFER_TO_GPU(buffer);

    bool ret = apir_buffer_cpy_tensor(gpu, BUFFER_TO_APIR_CONTEXT(buffer), src, dst);

    return ret;
}

static void ggml_backend_remoting_buffer_clear(ggml_backend_buffer_t buffer, uint8_t value) {
    virtgpu * gpu = BUFFER_TO_GPU(buffer);

    apir_buffer_clear(gpu, BUFFER_TO_APIR_CONTEXT(buffer), value);

    return;
}

static void ggml_backend_remoting_buffer_free_buffer(ggml_backend_buffer_t buffer) {
    virtgpu * gpu = BUFFER_TO_GPU(buffer);

    apir_buffer_free_buffer(gpu, BUFFER_TO_APIR_CONTEXT(buffer));

    ggml_backend_remoting_buffer_context * context = BUFFER_TO_GGML_CONTEXT(buffer);
    free(context);
    buffer->context = NULL;
}

const ggml_backend_buffer_i ggml_backend_remoting_buffer_interface = {
    /* .free_buffer     = */ ggml_backend_remoting_buffer_free_buffer,
    /* .get_base        = */ ggml_backend_remoting_buffer_get_base,
    /* .init_tensor     = */ NULL,
    /* .memset_tensor   = */ NULL,
    /* .set_tensor      = */ ggml_backend_remoting_buffer_set_tensor,
    /* .get_tensor      = */ ggml_backend_remoting_buffer_get_tensor,
    /* .cpy_tensor      = */ ggml_backend_remoting_buffer_cpy_tensor,
    /* .clear           = */ ggml_backend_remoting_buffer_clear,
    /* .reset           = */ NULL,
};

const ggml_backend_buffer_i ggml_backend_remoting_buffer_from_ptr_interface = {
    /* .free_buffer     = */ ggml_backend_remoting_buffer_free_buffer,
    /* .get_base        = */ ggml_backend_remoting_buffer_get_base,
    /* .init_tensor     = */ NULL,
    /* .memset_tensor   = */ NULL,
    /* .set_tensor      = */ ggml_backend_remoting_buffer_set_tensor_from_ptr,
    /* .get_tensor      = */ ggml_backend_remoting_buffer_get_tensor_from_ptr,
    /* .cpy_tensor      = */ ggml_backend_remoting_buffer_cpy_tensor,
    /* .clear           = */ ggml_backend_remoting_buffer_clear,
    /* .reset           = */ NULL,
};
