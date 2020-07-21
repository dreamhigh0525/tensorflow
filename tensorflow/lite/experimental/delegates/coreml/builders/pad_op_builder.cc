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
#include "tensorflow/lite/experimental/delegates/coreml/builders/pad_op_builder.h"

#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/experimental/delegates/coreml/builders/op_factory.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"

namespace tflite {
namespace delegates {
namespace coreml {

const char* PadOpBuilder::DebugName() {
  if (str_debug_name_[0]) return str_debug_name_;
  GetDebugName(padding_type_ == PadType::kPad ? "PadOpBuilder (PAD)"
                                              : "PadOpBuilder (MIRROR_PAD)",
               node_id_, str_debug_name_);
  return str_debug_name_;
}

CoreML::Specification::NeuralNetworkLayer* PadOpBuilder::Build() {
  layer_->set_name(DebugName());
  if (padding_type_ == PadType::kPad) {
    layer_->mutable_padding()->mutable_constant();
  } else if (padding_type_ == PadType::kMirrorPad) {
    layer_->mutable_padding()->mutable_reflection();
  }
  return layer_.release();
}

// padding is d x 2 tensor, where d is the dimension of input.
// only paddings for width and height are considered.
void PadOpBuilder::SetPadding(const TfLiteTensor* padding) {
  const int32_t* padding_data = GetTensorData<int32_t>(padding);
  for (int i = 1; i <= 2; ++i) {
    auto* borderamount = layer_->mutable_padding()
                             ->mutable_paddingamounts()
                             ->add_borderamounts();
    borderamount->set_startedgesize(padding_data[i * 2]);
    borderamount->set_endedgesize(padding_data[i * 2 + 1]);
  }
}

void PadOpBuilder::SetConstantValue(const TfLiteTensor* constant_value) {
  layer_->mutable_padding()->mutable_constant()->set_value(
      GetTensorData<float>(constant_value)[0]);
}

TfLiteStatus PadOpBuilder::RegisterInputs(const TfLiteIntArray* inputs,
                                          TfLiteContext* context) {
  if (!(inputs->size == 2 || inputs->size == 3)) {
    TF_LITE_KERNEL_LOG(context, "Wrong # of inputs to Padding!.");
    return kTfLiteError;
  }
  AddInput(inputs->data[0]);
  SetPadding(GetInput(context, tflite_node_, 1));
  if (inputs->size == 3) {
    SetConstantValue(GetInput(context, tflite_node_, 2));
  }

  return kTfLiteOk;
}

TfLiteStatus PadOpBuilder::RegisterOutputs(const TfLiteIntArray* outputs,
                                           TfLiteContext* context) {
  if (outputs->size != 1) {
    TF_LITE_KERNEL_LOG(context, "Wrong # of outputs to Padding!.");
    return kTfLiteError;
  }
  graph_builder_->AddTensorWithID(outputs->data[0], GetOutput(context));
  return kTfLiteOk;
}

OpBuilder* CreatePadOpBuilder(GraphBuilder* graph_builder) {
  return new PadOpBuilder(graph_builder, PadType::kPad);
}

OpBuilder* CreateMirrorPadOpBuilder(GraphBuilder* graph_builder) {
  return new PadOpBuilder(graph_builder, PadType::kMirrorPad);
}

}  // namespace coreml
}  // namespace delegates
}  // namespace tflite
