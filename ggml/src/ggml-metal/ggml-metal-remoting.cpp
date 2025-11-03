#include "ggml-metal-device.h"

#include "ggml-metal-impl.h"

#include "ggml-impl.h"

GGML_BACKEND_API void ggml_backend_metal_get_device_context(ggml_backend_dev_t dev,
							    bool *has_simdgroup_mm,
							    bool *has_simdgroup_reduction,
							    bool *use_bfloat);

GGML_BACKEND_API void
ggml_backend_metal_get_device_context(ggml_backend_dev_t dev,
				      bool *has_simdgroup_mm,
				      bool *has_simdgroup_reduction,
				      bool *use_bfloat) {
  ggml_metal_device_t dev = (ggml_metal_device_t)dev->context;

  *use_bfloat = dev->props.use_bfloat;
  *has_simdgroup_reduction = dev->props.has_simdgroup_reduction;
  *has_simdgroup_mm = dev->props.has_simdgroup_mm;
}
