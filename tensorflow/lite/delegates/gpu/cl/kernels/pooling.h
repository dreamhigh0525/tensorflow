/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_POOLING_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_POOLING_H_

#include "tensorflow/lite/delegates/gpu/cl/cl_kernel.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

class Pooling : public GPUOperation {
 public:
  Pooling(const OperationDef& definition, const Pooling2DAttributes& attr);
  Pooling(const OperationDef& definition, const Pooling3DAttributes& attr);

  absl::Status BindArguments() override;
  int3 GetGridSize() const override;
  absl::Status Compile(const CreationContext& creation_context) override;

  // Move only
  Pooling(Pooling&& kernel);
  Pooling& operator=(Pooling&& kernel);
  Pooling(const Pooling&) = delete;
  Pooling& operator=(const Pooling&) = delete;

 private:
  std::string GetAveragePoolingKernelCode(const OperationDef& op_def,
                                          bool stride_correction,
                                          const CLDevice& device);
  std::string GetMaxPoolingKernelCode(const OperationDef& op_def,
                                      bool stride_correction,
                                      bool output_indices);

  int4 stride_;
  int4 padding_;
  int4 kernel_size_;

  PoolingType type_;
  bool output_indices_;
};

Pooling CreatePooling(const OperationDef& definition,
                      const Pooling2DAttributes& attr);

Pooling CreatePooling(const OperationDef& definition,
                      const Pooling3DAttributes& attr);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_POOLING_H_
