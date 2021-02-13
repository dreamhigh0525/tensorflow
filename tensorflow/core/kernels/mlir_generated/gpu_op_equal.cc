/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include <complex>

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/kernels/mlir_generated/base_gpu_op.h"

namespace tensorflow {

GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, f16, i1, bool, Eigen::half);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, f32, i1, bool, float);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, f64, i1, bool, double);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, i1, i1, bool, bool);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, i8, i1, bool, int8);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, i16, i1, bool, int16);
// TODO(b/25387198): We cannot use a regular GPU kernel for int32.
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL2(Equal, i64, i1, bool, int64);

}  // namespace tensorflow
