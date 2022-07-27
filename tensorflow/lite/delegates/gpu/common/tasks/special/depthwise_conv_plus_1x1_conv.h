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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_COMMON_TASKS_SPECIAL_DEPTHWISE_CONV_PLUS_1X1_CONV_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_COMMON_TASKS_SPECIAL_DEPTHWISE_CONV_PLUS_1X1_CONV_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/precision.h"
#include "tensorflow/lite/delegates/gpu/common/selectors/subgraph.h"
#include "tensorflow/lite/delegates/gpu/common/task/tensor_desc.h"

namespace tflite {
namespace gpu {

class ThinPointwiseFuser {
 public:
  void Init(CalculationsPrecision precision);
  void AddConvNode(const GpuInfo& gpu_info,
                   const Convolution2DAttributes& attr);
  void AddReluNode(const ReLUAttributes& attr);
  void AddDepthwiseConvNode(const GpuInfo& gpu_info,
                            const TensorDescriptor& src_desc,
                            const DepthwiseConvolution2DAttributes& attr);
  GPUOperation Finalize(const GpuInfo& gpu_info,
                        const TensorDescriptor& dst_desc);

 private:
  void AddConvData(const Convolution2DAttributes& conv_attr);
  void AddDepthwiseConvData(const DepthwiseConvolution2DAttributes& dw_attr);
  void CreateConstantsGpuBuffer(const GpuInfo& gpu_info);
  OperationDef op_def_;
  Arguments args_;
  std::string code_;
  std::vector<std::string> outputs_;
  std::vector<float> gpu_data_;
  int weights_counter_ = 0;
};

GPUOperation CreateDepthwiseConvPlus1x1Conv(
    const OperationDef& definition, const GpuInfo& gpu_info,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr,
    ReLUAttributes* relu_attr_ptr = nullptr);

absl::Status TryDepthwiseConvPlus1x1Conv(
    const GpuInfo& gpu_info, CalculationsPrecision precision,
    const GraphFloat32& graph, NodeId first_node_id,
    const std::map<ValueId, TensorDescriptor>& tensor_descriptors,
    std::set<NodeId>* consumed_nodes, GPUOperationsSubgraph* gpu_subgraph);

}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_COMMON_TASKS_SPECIAL_DEPTHWISE_CONV_PLUS_1X1_CONV_H_
