/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/lite/delegates/utils/experimental/sample_vendor_delegate/sample_vendor_delegate.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/c/c_api_opaque.h"
#include "tensorflow/lite/c/c_api_types.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/delegates/utils/simple_opaque_delegate.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"

namespace tflite {
namespace example {
namespace {

class SampleVendorDelegateKernel : public SimpleOpaqueDelegateKernelInterface {
  bool IsExternalTensor(const TfLiteOpaqueTensor* opaque_tensor) const {
    return external_tensors_.count(opaque_tensor) != 0;
  }

  void DeriveExternalTensors() {
    for (const TfLiteOpaqueTensor* tensor : node_input_tensors_set_) {
      if (node_output_tensors_set_.count(tensor) == 0) {
        external_tensors_.insert(tensor);
      }
    }

    for (const TfLiteOpaqueTensor* tensor : node_output_tensors_set_) {
      if (node_input_tensors_set_.count(tensor) == 0) {
        external_tensors_.insert(tensor);
      }
    }
  }

 public:
  TfLiteStatus Init(TfLiteOpaqueContext* context,
                    const TfLiteOpaqueDelegateParams* params) override {
    if (params->delegate == nullptr) return kTfLiteDelegateError;

    context_ = context;
    builtin_code_.resize(params->nodes_to_replace->size);

    node_input_tensors_.resize(params->nodes_to_replace->size);
    node_output_tensors_.resize(params->nodes_to_replace->size);

    for (int i = 0; i < params->nodes_to_replace->size; ++i) {
      const int node_index = params->nodes_to_replace->data[i];

      TfLiteOpaqueNode* delegated_node = nullptr;
      TfLiteRegistrationExternal* delegated_node_registration = nullptr;
      TfLiteOpaqueContextGetNodeAndRegistration(
          context, node_index, &delegated_node, &delegated_node_registration);

      auto input_tensor1 = TfLiteOpaqueNodeGetInput(context, delegated_node, 0);
      node_input_tensors_[i].push_back(input_tensor1);
      node_input_tensors_set_.insert(input_tensor1);

      auto input_tensor2 = TfLiteOpaqueNodeGetInput(context, delegated_node, 1);
      node_input_tensors_[i].push_back(input_tensor2);
      node_input_tensors_set_.insert(input_tensor2);

      auto output_tensor =
          TfLiteOpaqueNodeGetOutput(context, delegated_node, 0);
      node_output_tensors_[i] = output_tensor;
      node_output_tensors_set_.insert(output_tensor);

      builtin_code_[i] =
          TfLiteRegistrationExternalGetBuiltInCode(delegated_node_registration);
    }

    // Determine which tensors are external (the TFLite runtime takes care
    // of them) so that we know which tensors are 'internal' to this delegate.
    // For the internal tensors we need to ensure they have memory allocated to
    // store their data, and take care of re-sizing etc.
    DeriveExternalTensors();

    return kTfLiteOk;
  }

  TfLiteStatus Prepare(TfLiteOpaqueContext* context,
                       TfLiteOpaqueNode* delegated_node) override {
    if (external_tensors_.empty()) return kTfLiteOk;

    const int kTheInputTensorSize =
        helpers::CalculateNumElements((*external_tensors_.begin()));
    for (std::vector<const TfLiteOpaqueTensor*>& vecs : node_input_tensors_) {
      for (const TfLiteOpaqueTensor* tensor : vecs) {
        if (IsExternalTensor(tensor)) continue;

        std::vector<float>& vec_memory = internal_tensors_memory_[tensor];
        vec_memory.resize(kTheInputTensorSize);
      }
    }
    for (const TfLiteOpaqueTensor* tensor : node_output_tensors_) {
      if (IsExternalTensor(tensor)) continue;

      std::vector<float>& vec_memory = internal_tensors_memory_[tensor];
      vec_memory.resize(kTheInputTensorSize);
    }

    return kTfLiteOk;
  }

  void ComputeImpl(float* input_1, float* input_2, float* output,
                   int builtin_code, int number_of_elements) {
    for (int i = 0; i < number_of_elements; ++i) {
      if (builtin_code == kTfLiteBuiltinAdd) {
        output[i] = input_1[i] + input_2[i];
      } else {
        output[i] = input_1[i] - input_2[i];
      }
    }
  }

  float* GetRawDataSource(TfLiteOpaqueContext* context,
                          const TfLiteOpaqueTensor* tensor) {
    if (IsExternalTensor(tensor)) {
      return reinterpret_cast<float*>(TfLiteOpaqueTensorData(tensor));
    } else {
      return internal_tensors_memory_[tensor].data();
    }
  }

  TfLiteStatus Eval(TfLiteOpaqueContext* context,
                    TfLiteOpaqueNode* delegated_node) override {
    for (int i = 0; i < node_input_tensors_.size(); ++i) {
      float* input1 = GetRawDataSource(context, node_input_tensors_[i][0]);
      float* input2 = GetRawDataSource(context, node_input_tensors_[i][1]);
      float* output = GetRawDataSource(context, node_output_tensors_[i]);
      // We assume that all input, output and intermediate tensors of the
      // delegated subgraph have the same size.
      ComputeImpl(input1, input2, output, builtin_code_[i],
                  helpers::CalculateNumElements(node_output_tensors_[i]));
    }
    return kTfLiteOk;
  }

 private:
  std::vector<std::vector<const TfLiteOpaqueTensor*>> node_input_tensors_;
  absl::flat_hash_set<const TfLiteOpaqueTensor*> node_input_tensors_set_;
  std::vector<const TfLiteOpaqueTensor*> node_output_tensors_;
  absl::flat_hash_set<const TfLiteOpaqueTensor*> node_output_tensors_set_;
  absl::flat_hash_set<const TfLiteOpaqueTensor*> external_tensors_;
  absl::flat_hash_map<const TfLiteOpaqueTensor*, std::vector<float>>
      internal_tensors_memory_;
  TfLiteOpaqueContext* context_;
  // Holds the builtin code of the ops.
  // builtin_code_[i] is the type of node at index 'i'
  std::vector<int> builtin_code_;
};
}  // namespace

int helpers::CalculateNumElements(const TfLiteOpaqueTensor* opaque_tensor) {
  int total_num_elements = 1;
  for (int i = 0; i < TfLiteOpaqueTensorNumDims(opaque_tensor); ++i) {
    total_num_elements *= TfLiteOpaqueTensorDim(opaque_tensor, i);
  }
  return total_num_elements;
}

bool SampleVendorDelegate::IsNodeSupportedByDelegate(
    const TfLiteRegistrationExternal* registration_external,
    const TfLiteOpaqueNode* node, TfLiteOpaqueContext* context) const {
  if (kTfLiteBuiltinAdd !=
          TfLiteRegistrationExternalGetBuiltInCode(registration_external) &&
      kTfLiteBuiltinSub !=
          TfLiteRegistrationExternalGetBuiltInCode(registration_external))
    return false;

  // This delegate only supports float32 types.
  for (int i = 0; i < TfLiteOpaqueNodeNumberOfInputs(node); ++i) {
    const TfLiteOpaqueTensor* tensor =
        TfLiteOpaqueNodeGetInput(context, node, i);
    if (TfLiteOpaqueTensorType(tensor) != kTfLiteFloat32) return false;
  }

  return true;
}

TfLiteStatus SampleVendorDelegate::Initialize(TfLiteOpaqueContext* context) {
  return kTfLiteOk;
}

const char* SampleVendorDelegate::Name() const {
  return kSampleVendorDelegateName;
}

std::unique_ptr<SimpleOpaqueDelegateKernelInterface>
SampleVendorDelegate::CreateDelegateKernelInterface() {
  return std::make_unique<SampleVendorDelegateKernel>();
}

}  // namespace example
}  // namespace tflite
