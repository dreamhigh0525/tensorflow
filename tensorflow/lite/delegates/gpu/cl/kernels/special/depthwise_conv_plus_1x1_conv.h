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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SPECIAL_DEPTHWISE_CONV_PLUS_1X1_CONV_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SPECIAL_DEPTHWISE_CONV_PLUS_1X1_CONV_H_

#include <vector>

#include "tensorflow/lite/delegates/gpu/cl/buffer.h"
#include "tensorflow/lite/delegates/gpu/cl/gpu_object.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"
#include "tensorflow/lite/delegates/gpu/cl/util.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/tensor.h"
#include "tensorflow/lite/delegates/gpu/common/types.h"

namespace tflite {
namespace gpu {
namespace cl {

class DepthwiseConvPlus1x1Conv : public GPUOperation {
 public:
  DepthwiseConvPlus1x1Conv() = default;
  int3 GetGridSize() const override;

  // Move only
  DepthwiseConvPlus1x1Conv(DepthwiseConvPlus1x1Conv&& operation);
  DepthwiseConvPlus1x1Conv& operator=(DepthwiseConvPlus1x1Conv&& operation);
  DepthwiseConvPlus1x1Conv(const DepthwiseConvPlus1x1Conv&) = delete;
  DepthwiseConvPlus1x1Conv& operator=(const DepthwiseConvPlus1x1Conv&) = delete;

 private:
  friend absl::Status CreateDepthwiseConvPlus1x1Conv(
      const CreationContext& creation_context, const OperationDef& definition,
      const DepthwiseConvolution2DAttributes& dw_attr,
      const Convolution2DAttributes& conv_attr,
      DepthwiseConvPlus1x1Conv* result);
  DepthwiseConvPlus1x1Conv(const OperationDef& definition,
                           const DepthwiseConvolution2DAttributes& dw_attr,
                           const Convolution2DAttributes& conv_attr,
                           const DeviceInfo& device_info);

  absl::Status UploadWeights(const DepthwiseConvolution2DAttributes& dw_attr,
                             const Convolution2DAttributes& conv_attr,
                             CLContext* context);

  std::string GenerateCode(const OperationDef& op_def,
                           const DepthwiseConvolution2DAttributes& dw_attr,
                           int result_depth, const DeviceInfo& device_info);

  DepthwiseConvolution2DAttributes dw_attr_;
};

bool IsDepthwiseConvPlus1x1ConvSupported(
    const CLDevice& device, const OperationDef& definition,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr);

absl::Status CreateDepthwiseConvPlus1x1Conv(
    const CreationContext& creation_context, const OperationDef& definition,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr, DepthwiseConvPlus1x1Conv* result);

}  // namespace cl
}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_CL_KERNELS_SPECIAL_DEPTHWISE_CONV_PLUS_1X1_CONV_H_
