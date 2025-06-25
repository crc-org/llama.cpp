#pragma once

#include "ggml-impl.h"
#include "ggml-backend-impl.h"
#include "ggml-backend.h"

struct ggml_backend_metal_device_context {
  bool has_simdgroup_mm;
  bool has_simdgroup_reduction;
  bool use_bfloat;
};


const struct ggml_backend_metal_device_context *get_metal_dev_context(const ggml_backend_dev_t dev);

bool ggml_metal_supports_op(const struct ggml_backend_metal_device_context * ctx_dev, const struct ggml_tensor * op);
