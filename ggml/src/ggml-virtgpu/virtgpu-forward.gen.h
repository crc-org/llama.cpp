#pragma once

/* device */
void                           apir_device_get_device_count(struct virtgpu * gpu);
int                            apir_device_get_count(struct virtgpu * gpu);
char *                         apir_device_get_name(struct virtgpu * gpu);
char *                         apir_device_get_description(struct virtgpu * gpu);
uint32_t                       apir_device_get_type(struct virtgpu * gpu);
void                           apir_device_get_memory(struct virtgpu * gpu, size_t * free, size_t * total);
bool                           apir_device_supports_op(struct virtgpu * gpu, const ggml_tensor * op);
apir_buffer_type_host_handle_t apir_device_get_buffer_type(struct virtgpu * gpu);
void                           apir_device_get_props(struct virtgpu * gpu,
                                                     bool *           async,
                                                     bool *           host_buffer,
                                                     bool *           buffer_from_host_ptr,
                                                     bool *           events);
apir_buffer_context_t          apir_device_buffer_from_ptr(struct virtgpu * gpu, size_t size, size_t max_tensor_size);

/* buffer-type */
char *                apir_buffer_type_get_name(struct virtgpu * gpu, apir_buffer_type_host_handle_t host_handle);
size_t                apir_buffer_type_get_alignment(struct virtgpu * gpu, apir_buffer_type_host_handle_t host_handle);
size_t                apir_buffer_type_get_max_size(struct virtgpu * gpu, apir_buffer_type_host_handle_t host_handle);
/* apir_buffer_type_is_host is deprecated. */
apir_buffer_context_t apir_buffer_type_alloc_buffer(struct virtgpu *               gpu,
                                                    apir_buffer_type_host_handle_t host_handle,
                                                    size_t                         size);
size_t                apir_buffer_type_get_alloc_size(struct virtgpu *               gpu,
                                                      apir_buffer_type_host_handle_t host_handle,
                                                      const ggml_tensor *            op);

/* buffer */
void * apir_buffer_get_base(struct virtgpu * gpu, apir_buffer_context_t * buffer_context);
void   apir_buffer_set_tensor(struct virtgpu *        gpu,
                              apir_buffer_context_t * buffer_context,
                              ggml_tensor *           tensor,
                              const void *            data,
                              size_t                  offset,
                              size_t                  size);
void   apir_buffer_get_tensor(struct virtgpu *        gpu,
                              apir_buffer_context_t * buffer_context,
                              const ggml_tensor *     tensor,
                              void *                  data,
                              size_t                  offset,
                              size_t                  size);
bool   apir_buffer_cpy_tensor(struct virtgpu *        gpu,
                              apir_buffer_context_t * buffer_context,
                              const ggml_tensor *     src,
                              const ggml_tensor *     dst);
void   apir_buffer_clear(struct virtgpu * gpu, apir_buffer_context_t * buffer_context, uint8_t value);
void   apir_buffer_free_buffer(struct virtgpu * gpu, apir_buffer_context_t * buffer_context);

/* backend */
int         apir_backend_initialize(struct virtgpu * gpu,
                                    void *           ggml_backend_reg_fct_p,
                                    uintptr_t *      out_device_handle,
                                    uint32_t *       out_backend_id);
ggml_status apir_backend_graph_compute(struct virtgpu * gpu,
                                       uintptr_t        device_handle,
                                       uint32_t         backend_id,
                                       ggml_cgraph *    cgraph);
void        apir_backend_cleanup(struct virtgpu * gpu, uintptr_t device_handle, uint32_t backend_id);
