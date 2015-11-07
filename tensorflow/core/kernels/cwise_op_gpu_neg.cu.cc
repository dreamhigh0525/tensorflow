#if GOOGLE_CUDA

#include "tensorflow/core/kernels/cwise_ops_gpu_common.cu.h"

namespace tensorflow {
namespace functor {
DEFINE_UNARY4(neg, float, double, int32, int64);
}  // namespace functor
}  // namespace tensorflow

#endif  // GOOGLE_CUDA
