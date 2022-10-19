/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)

#define EIGEN_USE_GPU

#include "tensorflow/core/framework/bfloat16.h"
#define SPECIALIZE_FOR_GPUS
#include "tensorflow/core/kernels/cast_op.h"
#undef SPECIALIZE_FOR_GPUS

namespace tensorflow {
namespace functor {

typedef Eigen::GpuDevice GPUDevice;

#if defined(MLIR_GENERATED_GPU_KERNELS_ENABLED)
CAST_FUNCTORS_SUBSET(GPUDevice);
#else
CAST_FUNCTORS(GPUDevice);
#endif

#define DEFINE(O, I) template struct CastFunctor<GPUDevice, O, I>

#define DEFINE_ALL_FROM(in_type)        \
  DEFINE(in_type, bool);                \
  DEFINE(in_type, uint8);               \
  DEFINE(in_type, uint16);              \
  DEFINE(in_type, uint32);              \
  DEFINE(in_type, uint64);              \
  DEFINE(in_type, int8);                \
  DEFINE(in_type, int16);               \
  DEFINE(in_type, int32);               \
  DEFINE(in_type, int64);               \
  DEFINE(in_type, Eigen::half);         \
  DEFINE(in_type, float);               \
  DEFINE(in_type, double);              \
  DEFINE(in_type, std::complex<float>); \
  DEFINE(in_type, std::complex<double>)

DEFINE(float, bfloat16);

#if defined(MLIR_GENERATED_GPU_KERNELS_ENABLED)

// The cast from float to double is still needed for resize_bilinear_op.cc
DEFINE(double, float);

#else

DEFINE_ALL_FROM(bool);
DEFINE_ALL_FROM(uint8);
DEFINE_ALL_FROM(uint16);
DEFINE_ALL_FROM(uint32);
DEFINE_ALL_FROM(uint64);
DEFINE_ALL_FROM(int8);
DEFINE_ALL_FROM(int16);
DEFINE_ALL_FROM(int32);
DEFINE_ALL_FROM(int64);
DEFINE_ALL_FROM(double);
DEFINE_ALL_FROM(std::complex<double>);
#endif

#define DEFINE_ALL_TO_FLOAT(out_type) \
  DEFINE(out_type, bool);             \
  DEFINE(out_type, uint8);            \
  DEFINE(out_type, uint16);           \
  DEFINE(out_type, uint32);           \
  DEFINE(out_type, uint64);           \
  DEFINE(out_type, int8);             \
  DEFINE(out_type, int16);            \
  DEFINE(out_type, int32);            \
  DEFINE(out_type, int64);            \
  DEFINE(out_type, Eigen::half);      \
  DEFINE(out_type, float);            \
  DEFINE(out_type, std::complex<float>)

#define DEFINE_ALL_TO_HALF(out_type) \
  DEFINE(out_type, bool);            \
  DEFINE(out_type, uint8);           \
  DEFINE(out_type, uint16);          \
  DEFINE(out_type, uint32);          \
  DEFINE(out_type, uint64);          \
  DEFINE(out_type, int8);            \
  DEFINE(out_type, int16);           \
  DEFINE(out_type, int32);           \
  DEFINE(out_type, int64);           \
  DEFINE(out_type, Eigen::half)

DEFINE_ALL_TO_HALF(bfloat16);

#if defined(MLIR_GENERATED_GPU_KERNELS_ENABLED)
// The cast from Eigen::half is still needed for depthwise_conv_grad_op.cc.
DEFINE(float, Eigen::half);
// The cast from float to float is still needed for resize_bilinear_op.cc.
DEFINE(float, float);
// The casts from complex to the complex element type is still needed for
// self_adjoint_eig_v2_op_gpu.cc
DEFINE(std::complex<float>, float);
DEFINE(std::complex<double>, double);
#else
DEFINE_ALL_TO_HALF(Eigen::half);
DEFINE_ALL_TO_FLOAT(float);
DEFINE_ALL_TO_FLOAT(std::complex<float>);
#endif

#undef DEFINE_ALL_TO_FLOAT
#undef DEFINE_ALL_TO_HALF
#undef DEFINE_ALL_FROM
#undef DEFINE

}  // end namespace functor
}  // end namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
