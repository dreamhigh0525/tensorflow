/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/kernels/cast_op_impl.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

std::function<void(OpKernelContext*, const Tensor&, Tensor*)>
GetCpuCastFromInt64(DataType dst_dtype) {
  CURRY_TYPES3(CAST_CASE, CPUDevice, int64);
  return nullptr;
}

#if GOOGLE_CUDA
std::function<void(OpKernelContext*, const Tensor&, Tensor*)>
GetGpuCastFromInt64(DataType dst_dtype) {
  CURRY_TYPES3(CAST_CASE, GPUDevice, int64);
  return nullptr;
}
#endif  // GOOGLE_CUDA

}  // namespace tensorflow
