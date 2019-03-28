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

#include "tensorflow/lite/delegates/gpu/metal/api.h"

#include <vector>

#include "tensorflow/lite/delegates/gpu/common/model.h"
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/metal/compiled_model.h"
#include "tensorflow/lite/delegates/gpu/metal/compute_task_descriptor.h"
#include "tensorflow/lite/delegates/gpu/metal/environment.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/abs.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/add.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/concat.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/convolution.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/convolution1x1.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/convolution_generic.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/depth_wise_conv3x3_stride1x1.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/depth_wise_convolution.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/fully_connected.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/max_unpooling.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/mul.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/padding.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/pooling.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/prelu.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/relu.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/reshape.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/sigmoid.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/slice.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/softmax.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/sub.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/transpose_conv.h"
#include "tensorflow/lite/delegates/gpu/metal/kernels/upsample.h"
#include "tensorflow/lite/delegates/gpu/metal/runtime_options.h"

namespace tflite {
namespace gpu {
namespace metal {
namespace {

std::vector<ComputeTaskDescriptorPtr> SelectConvolution(
    int id, ValueId input_id, ValueId output_id,
    const Convolution2DAttributes& attr, const metal::RuntimeOptions& options) {
  if (GetAppleSocVersion() >= 11) {
    bool conv1x1 = attr.weights.shape.h == 1 && attr.weights.shape.w == 1 &&
                   attr.strides.h == 1 && attr.strides.w == 1 &&
                   attr.dilations.h == 1 && attr.dilations.w == 1 &&
                   attr.padding.prepended.h == 0 &&
                   attr.padding.prepended.w == 0 &&
                   attr.padding.appended.h == 0 && attr.padding.appended.w == 0;
    if (conv1x1) {
      return Convolution1x1(id, input_id, output_id, attr, options);
    } else {
      return ConvolutionGeneric(id, input_id, output_id, attr, options);
    }
  } else {
    return Convolution(id, input_id, output_id, attr, options);
  }
}

std::vector<ComputeTaskDescriptorPtr> SelectDepthWiseConv(
    int id, ValueId input_id, ValueId output_id,
    const DepthwiseConvolution2DAttributes& attr,
    const metal::RuntimeOptions& options) {
  if (CheckDepthWiseConv3x3Stride1x1Support(attr)) {
    return DepthWiseConv3x3Stride1x1(id, input_id, output_id, attr, options);
  } else {
    return DepthWiseConvolution(id, input_id, output_id, attr, options);
  }
}

}  // namespace

Status Compile(const GraphFloat32& graph, const RuntimeOptions& options,
               CompiledModel* compiled_model) {
  for (const auto& node : graph.nodes()) {
    int node_id = static_cast<int>(node->id);
    std::vector<ValueId> inputs;
    for (auto& input : graph.FindInputs(node->id)) {
      inputs.push_back(static_cast<ValueId>(input->id));
    }
    std::vector<ValueId> outputs;
    for (auto& output : graph.FindOutputs(node->id)) {
      outputs.push_back(static_cast<ValueId>(output->id));
    }

    std::vector<ComputeTaskDescriptorPtr> tasks;
    switch (OperationTypeFromString(node->operation.type)) {
      case OperationType::ABS:
        tasks = Abs(node_id, inputs[0], outputs[0]);
        break;
      case OperationType::ADD:
        tasks = AddTable(node_id, inputs, outputs[0]);
        break;
      case OperationType::CONCAT: {
        std::vector<BHWC> input_shapes;
        for (auto& input : graph.FindInputs(node->id)) {
          input_shapes.push_back(input->tensor.shape);
        }
        tasks =
            Concat(node_id, inputs, outputs[0],
                   absl::any_cast<ConcatAttributes>(node->operation.attributes),
                   input_shapes);
      } break;
      case OperationType::CONVOLUTION_2D:
        tasks = SelectConvolution(
            node_id, inputs[0], outputs[0],
            absl::any_cast<Convolution2DAttributes>(node->operation.attributes),
            options);
        break;
      case OperationType::CONVOLUTION_TRANSPOSED:
        tasks = ConvolutionTransposed(
            node_id, inputs[0], outputs[0],
            absl::any_cast<ConvolutionTransposedAttributes>(
                node->operation.attributes),
            options);
        break;
      case OperationType::DEPTHWISE_CONVOLUTION:
        tasks = SelectDepthWiseConv(
            node_id, inputs[0], outputs[0],
            absl::any_cast<DepthwiseConvolution2DAttributes>(
                node->operation.attributes),
            options);
        break;
      case OperationType::FULLY_CONNECTED:
        tasks = FullyConnected(node_id, inputs[0], outputs[0],
                               absl::any_cast<FullyConnectedAttributes>(
                                   node->operation.attributes),
                               options);
        break;
      case OperationType::MAX_UNPOOLING_2D:
        tasks = MaxUnpooling(node_id, inputs[0], inputs[1], outputs[0],
                             absl::any_cast<MaxUnpooling2DAttributes>(
                                 node->operation.attributes));
        break;
      case OperationType::MULTIPLY_SCALAR:
        tasks = Multiply(node_id, inputs[0], outputs[0],
                         absl::any_cast<MultiplyScalarAttributes>(
                             node->operation.attributes),
                         options);
        break;
      case OperationType::PAD:
        tasks =
            Padding(node_id, inputs[0], outputs[0],
                    absl::any_cast<PadAttributes>(node->operation.attributes));
        break;
      case OperationType::POOLING_2D:
        tasks = Pooling(
            node_id, inputs[0], outputs,
            absl::any_cast<Pooling2DAttributes>(node->operation.attributes));
        break;
      case OperationType::PRELU:
        tasks =
            PReLU(node_id, inputs[0], outputs[0],
                  absl::any_cast<PReLUAttributes>(node->operation.attributes),
                  options);
        break;
      case OperationType::RELU:
        tasks =
            ReLU(node_id, inputs[0], outputs[0],
                 absl::any_cast<ReLUAttributes>(node->operation.attributes));
        break;
      case OperationType::RESHAPE:
        tasks = Reshape(
            node_id, inputs[0], outputs[0],
            absl::any_cast<ReshapeAttributes>(node->operation.attributes)
                .new_shape);
        break;
      case OperationType::SIGMOID:
        tasks = Sigmoid(node_id, inputs[0], outputs[0]);
        break;
      case OperationType::SLICE:
        tasks =
            Slice(node_id, inputs[0], outputs[0],
                  absl::any_cast<SliceAttributes>(node->operation.attributes));
        break;
      case OperationType::SOFT_MAX:
        tasks = Softmax(node_id, inputs[0], outputs[0],
                        graph.FindInputs(node->id)[0]->tensor.shape.c, options);
        break;
      case OperationType::SUB:
        tasks = Sub(node_id, inputs, outputs[0]);
        break;
      case OperationType::UPSAMPLE_2D:
        tasks = Upsample(
            node_id, inputs[0], outputs[0],
            absl::any_cast<Upsample2DAttributes>(node->operation.attributes));
        break;
      case OperationType::APPLY_MASK:
      case OperationType::BATCH_NORMALIZATION:
      case OperationType::CONST:
      case OperationType::COS:
      case OperationType::LOG:
      case OperationType::LSTM:
      case OperationType::MUL:
      case OperationType::RESIZE:
      case OperationType::RSQRT:
      case OperationType::SIN:
      case OperationType::SQRT:
      case OperationType::SQUARE:
      case OperationType::TANH:
      case OperationType::UNKNOWN:
        return UnimplementedError("Unsupported op: " + node->operation.type);
    }
    compiled_model->insert(compiled_model->end(), tasks.begin(), tasks.end());
  }
  return OkStatus();
}

}  // namespace metal
}  // namespace gpu
}  // namespace tflite
