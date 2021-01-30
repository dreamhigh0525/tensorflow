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

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/kernels/mlir_generated/gpu_ops_base.h"

namespace tensorflow {

GENERATE_AND_REGISTER_UNARY_KERNEL(Abs, f16, DT_HALF, Eigen::half);
GENERATE_AND_REGISTER_UNARY_KERNEL(Abs, f32, DT_FLOAT, float);
GENERATE_AND_REGISTER_UNARY_KERNEL(Abs, f64, DT_DOUBLE, double);
// TODO(b/25387198): Add an int32 kernel.
GENERATE_AND_REGISTER_UNARY_KERNEL(Abs, i64, DT_INT64, int64);

}  // namespace tensorflow
