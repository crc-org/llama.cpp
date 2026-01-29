/*
 * Minimal APIR Implementation for Backend Refactoring
 *
 * This provides the basic APIR encoder/decoder functions needed
 * by the new backend architecture.
 */

#pragma once

/* Include virtgpu-interface.h first to define virtgpu_shmem */
#include "virtgpu-interface.h"
#include "apir-windows.h"
#include "backend/shared/apir_cs.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Missing APIR types */
#ifndef APIR_BUFFER_TYPE_HOST_HANDLE_T_DEFINED
#define APIR_BUFFER_TYPE_HOST_HANDLE_T_DEFINED
typedef uint64_t apir_buffer_type_host_handle_t;
#endif

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* Basic encoder functions */
static inline apir_encoder* apir_encoder_init(void* buffer, size_t size) {
    apir_encoder* enc = (apir_encoder*)malloc(sizeof(apir_encoder));
    if (!enc) return NULL;

    enc->cur = (char*)buffer;
    enc->start = (char*)buffer;
    enc->end = (char*)buffer + size;
    enc->fatal = false;

    return enc;
}

static inline void apir_encoder_deinit(apir_encoder* enc) {
    if (enc) {
        free(enc);
    }
}

static inline size_t apir_encoder_get_encoded_size(apir_encoder* enc) {
    if (!enc) return 0;
    return enc->cur - enc->start;
}

static inline int apir_encode_uint32_t(apir_encoder* enc, uint32_t* value) {
    if (!enc || !value || enc->cur + sizeof(uint32_t) > enc->end) {
        if (enc) enc->fatal = true;
        return -1;
    }

    memcpy(enc->cur, value, sizeof(uint32_t));
    enc->cur += sizeof(uint32_t);
    return 0;
}

static inline int apir_encode_int32_t(apir_encoder* enc, int32_t* value) {
    if (!enc || !value || enc->cur + sizeof(int32_t) > enc->end) {
        if (enc) enc->fatal = true;
        return -1;
    }

    memcpy(enc->cur, value, sizeof(int32_t));
    enc->cur += sizeof(int32_t);
    return 0;
}

/* Basic decoder functions */
static inline apir_decoder* apir_decoder_init(const void* buffer, size_t size) {
    apir_decoder* dec = (apir_decoder*)malloc(sizeof(apir_decoder));
    if (!dec) return NULL;

    dec->cur = (const char*)buffer;
    dec->end = (const char*)buffer + size;
    dec->fatal = false;

    return dec;
}

static inline void apir_decoder_deinit(apir_decoder* dec) {
    if (dec) {
        free(dec);
    }
}


/* Missing APIR functions needed by ggml backend */
static inline apir_buffer_context_t apir_device_buffer_from_ptr(virtgpu* gpu, void* ptr, size_t size) {
    UNUSED(gpu);
    UNUSED(size);

    apir_buffer_context_t context;
    context.host_handle = 0;
    context.shmem.mmap_ptr = ptr;  // Use the provided pointer directly
    context.shmem.res_id = 0;      // Initialize res_id field
    context.shmem.mmap_size = size;
    context.shmem.backend_data = NULL;
    context.buft_host_handle = 0;
    return context;
}

#ifdef __cplusplus
}
#endif
