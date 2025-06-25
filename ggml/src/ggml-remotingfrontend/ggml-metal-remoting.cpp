#include "ggml-remoting.h"
#include "ggml-metal-remoting.h"

const struct ggml_backend_metal_device_context *get_metal_dev_context(const ggml_backend_dev_t dev) {
  static struct ggml_backend_metal_device_context metal_dev_ctx;
  static bool has_metal_dev_ctx = false;

  if (has_metal_dev_ctx) {
    return &metal_dev_ctx;
  }

  struct virtgpu *gpu = DEV_TO_GPU(dev);

  apir_metal_get_device_context(gpu, &metal_dev_ctx);

  return &metal_dev_ctx;
}


bool ggml_metal_supports_op(const struct ggml_backend_metal_device_context * ctx_dev, const struct ggml_tensor * op) {
    const bool has_simdgroup_mm        = ctx_dev->has_simdgroup_mm;
    const bool has_simdgroup_reduction = ctx_dev->has_simdgroup_reduction;
    const bool use_bfloat              = ctx_dev->use_bfloat;

    if (!use_bfloat) {
        for (size_t i = 0, n = 3; i < n; ++i) {
            if (op->src[i] != NULL && op->src[i]->type == GGML_TYPE_BF16) {
                return false;
            }
        }
    }

    switch (op->op) {
        case GGML_OP_UNARY:
            switch (ggml_get_unary_op(op)) {
                case GGML_UNARY_OP_TANH:
                case GGML_UNARY_OP_RELU:
                case GGML_UNARY_OP_SIGMOID:
                case GGML_UNARY_OP_GELU:
                case GGML_UNARY_OP_GELU_ERF:
                case GGML_UNARY_OP_GELU_QUICK:
                case GGML_UNARY_OP_SILU:
                case GGML_UNARY_OP_ELU:
                case GGML_UNARY_OP_NEG:
                    return ggml_is_contiguous(op->src[0]) && op->src[0]->type == GGML_TYPE_F32;
                default:
                    return false;
            }
        case GGML_OP_NONE:
        case GGML_OP_RESHAPE:
        case GGML_OP_VIEW:
        case GGML_OP_TRANSPOSE:
        case GGML_OP_PERMUTE:
        case GGML_OP_CONCAT:
            return true;
        case GGML_OP_ADD:
        case GGML_OP_SUB:
        case GGML_OP_MUL:
        case GGML_OP_DIV:
            return op->src[0]->type == GGML_TYPE_F32;
        case GGML_OP_ACC:
        case GGML_OP_REPEAT:
        case GGML_OP_SCALE:
        case GGML_OP_CONV_TRANSPOSE_1D:
            return true;
        case GGML_OP_CLAMP:
            return op->src[0]->type == GGML_TYPE_F32;
        case GGML_OP_SQR:
        case GGML_OP_SQRT:
        case GGML_OP_SIN:
        case GGML_OP_COS:
            return ggml_is_contiguous(op->src[0]) && op->src[0]->type == GGML_TYPE_F32;
        case GGML_OP_LOG:
            return false; // TODO: implement
        case GGML_OP_SUM_ROWS:
        case GGML_OP_MEAN:
        case GGML_OP_SOFT_MAX:
        case GGML_OP_GROUP_NORM:
            return has_simdgroup_reduction && ggml_is_contiguous(op->src[0]);
        case GGML_OP_RMS_NORM:
        case GGML_OP_L2_NORM:
            return has_simdgroup_reduction && (op->ne[0] % 4 == 0 && ggml_is_contiguous_1(op->src[0]));
        case GGML_OP_ARGMAX:
            return true;
        case GGML_OP_NORM:
            return has_simdgroup_reduction && (op->ne[0] % 4 == 0 && ggml_is_contiguous_1(op->src[0]));
        case GGML_OP_ROPE:
            return true;
        case GGML_OP_IM2COL:
            return op->src[0]->type == GGML_TYPE_F16;
        case GGML_OP_POOL_1D:
            return false;
        case GGML_OP_UPSCALE:
            return op->src[0]->type == GGML_TYPE_F32 && op->op_params[0] == GGML_SCALE_MODE_NEAREST;
        case GGML_OP_POOL_2D:
        case GGML_OP_PAD:
        case GGML_OP_PAD_REFLECT_1D:
        case GGML_OP_TIMESTEP_EMBEDDING:
        case GGML_OP_ARGSORT:
        case GGML_OP_LEAKY_RELU:
            return op->src[0]->type == GGML_TYPE_F32;
        case GGML_OP_ARANGE:
            return true;
        case GGML_OP_FLASH_ATTN_EXT:
            if (op->src[0]->ne[0] == 32) {
                // head size == 32 (e.g. bert-bge-small)
                // TODO: not sure if it is worth adding kernels for this size
                return false;
            }
            if (op->src[0]->ne[0] == 576) {
                // DeepSeek sizes
                // TODO: disabled for now, until optmized
                return false;
            }
            if (op->src[1]->type != op->src[2]->type) {
                return false;
            }
            return has_simdgroup_mm; // TODO: over-restricted for vec-kernels
        case GGML_OP_SSM_CONV:
        case GGML_OP_SSM_SCAN:
        case GGML_OP_RWKV_WKV6:
        case GGML_OP_RWKV_WKV7:
            return true;
        case GGML_OP_MUL_MAT:
        case GGML_OP_MUL_MAT_ID:
            return has_simdgroup_reduction &&
                (op->src[0]->type != GGML_TYPE_F32 || op->src[1]->type == GGML_TYPE_F32);
        case GGML_OP_CPY:
        case GGML_OP_DUP:
        case GGML_OP_CONT:
            {
                switch (op->src[0]->type) {
                    case GGML_TYPE_F32:
                        switch (op->type) {
                           case GGML_TYPE_F32:
                           case GGML_TYPE_F16:
                           case GGML_TYPE_BF16:
                           case GGML_TYPE_Q8_0:
                           case GGML_TYPE_Q4_0:
                           case GGML_TYPE_Q4_1:
                           case GGML_TYPE_Q5_0:
                           case GGML_TYPE_Q5_1:
                           case GGML_TYPE_IQ4_NL:
                                return true;
                           default:
                                return false;
                        }
                    case GGML_TYPE_F16:
                        switch (op->type) {
                            case GGML_TYPE_F32:
                            case GGML_TYPE_F16:
                                return true;
                            default:
                                return false;
                        }
                    case GGML_TYPE_BF16:
                        switch (op->type) {
                            case GGML_TYPE_F32:
                            case GGML_TYPE_BF16:
                                return true;
                            default:
                                return false;
                        }
                    case GGML_TYPE_Q4_0:
                    case GGML_TYPE_Q4_1:
                    case GGML_TYPE_Q5_0:
                    case GGML_TYPE_Q5_1:
                    case GGML_TYPE_Q8_0:
                        switch (op->type) {
                            case GGML_TYPE_F32:
                            case GGML_TYPE_F16:
                                return true;
                            default:
                                return false;
                        }
                    default:
                        return false;
                };
            }
        case GGML_OP_SET:
            {
                switch (op->src[0]->type) {
                    case GGML_TYPE_F32:
                    case GGML_TYPE_I32:
                        return true;
                    default:
                        return false;
                };
            }
        case GGML_OP_DIAG_MASK_INF:
        case GGML_OP_GET_ROWS:
            {
                return op->ne[3] == 1;
            }
        default:
            return false;
    }
}
