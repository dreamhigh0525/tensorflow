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
#include "tensorflow/core/kernels/mlir_generated/base_gpu_op.h"

namespace tensorflow {

GENERATE_AND_REGISTER_BINARY_GPU_KERNEL(AddV2, DT_HALF);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL(AddV2, DT_FLOAT);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL(AddV2, DT_DOUBLE);
GENERATE_AND_REGISTER_BINARY_GPU_KERNEL(AddV2, DT_INT64);

// Add is the same as AddV2 except for strings, which we do not support on gpu.
REGISTER_ALIASED_GPU_KERNEL(Add, AddV2, DT_HALF, DT_HALF);
REGISTER_ALIASED_GPU_KERNEL(Add, AddV2, DT_FLOAT, DT_FLOAT);
REGISTER_ALIASED_GPU_KERNEL(Add, AddV2, DT_DOUBLE, DT_DOUBLE);
REGISTER_ALIASED_GPU_KERNEL(Add, AddV2, DT_INT64, DT_INT64);

}  // namespace tensorflow
