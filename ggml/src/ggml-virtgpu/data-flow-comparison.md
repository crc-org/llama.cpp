# Complete Data Flow: Linux vs Windows

## GGML Operation: `ggml_backend_graph_compute(backend, cgraph)`

### Linux DRM Path:
```
1. GGML Layer:
   ggml_backend_graph_compute(backend, cgraph)

2. Backend Layer:
   ggml_backend_virtgpu_graph_compute()

3. APIR Layer:
   remote_call_prepare(gpu, APIR_COMMAND_TYPE_FORWARD, 0)
   ├── buffer = gpu->data_shmem.ptr              [DRM GEM buffer]
   ├── encoder = apir_encoder_init(buffer, size)
   └── apir_encode_cgraph(encoder, cgraph)       [Binary APIR data]

4. Transport Layer:
   remote_call(gpu, encoder, &decoder, ...)
   ├── virtgpu_ioctl(gpu, DRM_IOCTL_EXECBUF, req) [DRM ioctl]
   ├── [Data travels via virtio-gpu hypervisor]
   └── decoder = apir_decoder_init(reply_buffer)   [Response from hypervisor]

5. Response:
   apir_decode_result(decoder, &result)
   ├── remote_call_finish()
   └── return result to GGML
```

### Windows winApiRmt Path:
```
1. GGML Layer:
   ggml_backend_graph_compute(backend, cgraph)    [SAME]

2. Backend Layer:
   ggml_backend_virtgpu_graph_compute()           [SAME]

3. APIR Layer:
   remote_call_prepare(gpu, APIR_COMMAND_TYPE_FORWARD, 0)
   ├── buffer = gpu->data_shmem.mapped_ptr       [winApiRmt file buffer]
   ├── encoder = apir_encoder_init(buffer, size) [SAME APIR encoder!]
   └── apir_encode_cgraph(encoder, cgraph)       [SAME Binary APIR data!]

4. Transport Layer:
   remote_call(gpu, encoder, &decoder, ...)
   ├── winapi_send_apir_command(handle, data, size) [winApiRmt call]
   ├── [Data travels via Hyper-V socket/TCP]
   └── decoder = apir_decoder_init(reply_buffer)    [Response from Windows]

5. Response:
   apir_decode_result(decoder, &result)           [SAME]
   ├── remote_call_finish()                      [SAME]
   └── return result to GGML                     [SAME]
```

## Key Insight: Protocol Preservation

### What Stays The Same:
- **GGML Interface**: `ggml_backend_graph_compute()`
- **APIR Protocol**: Binary encoding/decoding
- **Function Signatures**: `remote_call_prepare()`, `remote_call()`, etc.
- **Response Handling**: Same decoder logic

### What Changes:
- **Memory Allocation**: DRM GEM vs File-backed
- **Transport**: DRM ioctl vs winApiRmt socket
- **Connection**: `/dev/dri/renderD*` vs Hyper-V socket
- **Dependencies**: libdrm vs json-c

### Result:
**Same GGML application works on both platforms without modification!**