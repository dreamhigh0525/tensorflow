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

#ifndef TENSORFLOW_LITE_DELEGATES_NNAPI_NNAPI_DELEGATE_KERNEL_H_
#define TENSORFLOW_LITE_DELEGATES_NNAPI_NNAPI_DELEGATE_KERNEL_H_

#include <map>
#include <memory>

#include "tensorflow/lite/allocation.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/delegates/nnapi/nnapi_delegate.h"
#include "tensorflow/lite/nnapi/nnapi_implementation.h"

namespace tflite {
namespace delegate {
namespace nnapi {

constexpr int32_t kMinSdkVersionForNNAPI = 27;
constexpr int32_t kMinSdkVersionForNNAPI11 = 28;
constexpr int32_t kMinSdkVersionForNNAPI12 = 29;

// Track tensor indices to NN API tensor indices mapping.
class OperandMapping {
 public:
  // Given a TFLite index return the ANN index. If it doesn't exist
  // return -1.
  int lite_index_to_ann(int index) const {
    if (index >= 0 && index < lite_tensor_to_ann_tensor_.size())
      return lite_tensor_to_ann_tensor_[index];
    else
      return -1;
  }

  // NN API uses non tensor operands instead of structs. This creates one
  // and returns the index. It uses a std::vector and resizes it as needed
  // keeping -1 to unmapped values. Intermediate tensors likely will not
  // be mapped.
  int add_new_non_tensor_operand() { return next_ann_tensor_index_++; }

  // This call is necessary for input operands generated by the delegate
  // to map constant inputs not present in TFLite but required by NNAPI,
  // for example when splitting one input in several ones.
  int add_delegate_generated_input_ann_tensors_operand() {
    return next_ann_tensor_index_++;
  }

  // Add a new mapping from `tflite_index` and return the NN API tensor index.
  int add_new_ann_tensor_index(int tflite_index) {
    if (tflite_index >= lite_tensor_to_ann_tensor_.size()) {
      lite_tensor_to_ann_tensor_.resize(tflite_index + 1, -1);
    }
    const int new_tensor_index = next_ann_tensor_index_++;
    lite_tensor_to_ann_tensor_[tflite_index] = new_tensor_index;
    return new_tensor_index;
  }

  // Given a TFLite index returns a TFLite type to which a tensor must be
  // converted during copying the data to the memory allocated for NN API.
  // kTfLiteNoType means no conversion is needed.
  TfLiteType lite_index_to_ann_type_conversion(int index) const {
    if (index >= 0 && index < index_to_type_conversion_.size())
      return index_to_type_conversion_[index];
    else
      return kTfLiteNoType;
  }

  // Add a new mapping from TFLite index to a type conversion.
  void add_type_conversion(int tflite_index, TfLiteType tflite_type) {
    if (tflite_index >= index_to_type_conversion_.size()) {
      index_to_type_conversion_.resize(tflite_index + 1, kTfLiteNoType);
    }
    index_to_type_conversion_[tflite_index] = tflite_type;
  }

 private:
  // Next index of ann tensor
  int next_ann_tensor_index_ = 0;

  // Mapping from lite index. Use a std::vector for speed and code size
  // rather than a map.
  std::vector<int> lite_tensor_to_ann_tensor_;
  // Mapping from lite index to a type which tensor must be converted to during
  // the copying of the data to the memory allocated for NN API. kTfLiteNoType
  // means no conversion is needed. Use an std::vector for speed and code size
  // rather than a map.
  std::vector<TfLiteType> index_to_type_conversion_;
};

class NNAPIOpBuilder;

// The kernel that represents the node sub set of TF Lite being run on NN API.
struct NNAPIOpMappingArgs {
  TfLiteContext* context;
  NNAPIOpBuilder* builder;
  TfLiteNode* node;
  std::vector<int>* model_state_outputs;
  std::vector<int>* model_state_tfl_inputs;
  std::vector<std::tuple<int, int>>* feedback_loops;
  int* nnapi_errno;
};

// RAII NN API Model Destructor for use with std::unique_ptr
struct NNFreeModel {
  void operator()(ANeuralNetworksModel* model) {
    NnApiImplementation()->ANeuralNetworksModel_free(model);
  }
};
// RAII NN API Compilation Destructor for use with std::unique_ptr
struct NNFreeCompilation {
  void operator()(ANeuralNetworksCompilation* model) {
    NnApiImplementation()->ANeuralNetworksCompilation_free(model);
  }
};

// Manage NNAPI shared memory handle
class NNMemory {
 public:
#ifdef TFLITE_NNAPI_ALLOW_MMAP_SHARING
  NNMemory(const NnApi* nnapi, const char* name, size_t size) {
    if (name && size > 0) {
      nnapi_ = nnapi;
      byte_size_ = size;
      fd_ = nnapi_->ASharedMemory_create(name, size);
      data_ptr_ = reinterpret_cast<uint8_t*>(
          mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
      nnapi_->ANeuralNetworksMemory_createFromFd(size, PROT_READ | PROT_WRITE,
                                                 fd_, 0, &nn_memory_handle_);
    }
  }
#else
  NNMemory(const NnApi* /*nnapi*/, const char* /*name*/, size_t /*size*/) {}
#endif

  ~NNMemory() {
#ifdef TFLITE_NNAPI_ALLOW_MMAP_SHARING
    if (data_ptr_) {
      munmap(data_ptr_, byte_size_);
    }
    if (nn_memory_handle_) {
      nnapi_->ANeuralNetworksMemory_free(nn_memory_handle_);
    }
    if (fd_ > 0) close(fd_);
#endif
  }

  ANeuralNetworksMemory* get_handle() { return nn_memory_handle_; }
  uint8_t* get_data_ptr() { return data_ptr_; }

 private:
#ifdef TFLITE_NNAPI_ALLOW_MMAP_SHARING
  const NnApi* nnapi_;
  int fd_ = 0;
  size_t byte_size_ = 0;
#endif
  uint8_t* data_ptr_ = nullptr;
  ANeuralNetworksMemory* nn_memory_handle_ = nullptr;
};

enum class NNAPIValidationFailureType : int {
  // The operator is not supported by either NNAPI or the NNAPI Delegate.
  kUnsupportedOperator = 0,
  // The given operation or operands are not supported on the specified
  // Android SDK version. The min supported version is specified in the
  // validation failure message.
  kUnsupportedAndroidVersion = 1,
  // The version of the operator (value of TfLiteRegistration::version)
  // for the given op is not supported. The max supported version
  // is specified in the validation failure message.
  // For more details on each operator version see
  // the GetBuiltinOperatorVersion function in
  // third_party/tensorflow/lite/tools/versioning/op_version.cc.
  kUnsupportedOperatorVersion = 2,
  // The given input operand type is not supported for the current combination
  // of operator type and sdk version.
  kUnsupportedInputType = 3,
  // When using NN API version 1.0 or 1.1, the condition
  //   input_scale * filter_scale < output_scale
  // must be true for quantized versions of the following ops:
  // * CONV_2D
  // * DEPTHWISE_CONV_2D
  // * FULLY_CONNECTED (where filter actually stands for weights)
  // The condition is relaxed and no longer required since version 1.2.
  kNotRestrictedScaleCompliant = 4,
  // The given output operand type is not supported for the current combination
  // of operator type and sdk version.
  kUnsupportedOutputType = 5,
  // The size of the operand tensor is too large.
  kUnsupportedOperandSize = 6,
  // The value of one of the operands or of a combination of operands is
  // not supported. Details are provided in the failure message.
  kUnsupportedOperandValue = 7,
  // The combination of float inputs and quantized weights or filters
  // is not supported
  kUnsupportedHybridOperator = 8,
  // The quantization type (for example per-channel quantization) is not
  // supported.
  kUnsupportedQuantizationType = 9,
  // The accelerated version of operation requires a specific operand to be
  // specified.
  kMissingRequiredOperand = 10,
  // The rank of the operand is not supported. Details in the failure message.
  kUnsupportedOperandRank = 11,
  // The input tensor cannot be dynamically-sized.
  kInputTensorShouldHaveConstantShape = 12,
  // The operator has a different number of inputs of the one or ones that
  // are supported by NNAPI.
  kUnsupportedOperatorVariant = 13,
  // The accelerated version of the operator cannot specify an activation
  // function.
  kNoActivationExpected = 14,
  // Quantization scale and/or zero point are not in the supported value(s)
  // for the accelerated operation.
  kUnsupportedQuantizationParameters = 15,
};

struct NNAPIValidationFailure {
  NNAPIValidationFailureType type;
  std::string message;

  NNAPIValidationFailure(NNAPIValidationFailureType type, const char* message)
      : type(type), message(message) {}
};

// The kernel that represents the node sub set of TF Lite being run on NN API.
class NNAPIDelegateKernel {
 public:
  NNAPIDelegateKernel() { nnapi_ = NnApiImplementation(); }
  ~NNAPIDelegateKernel() {
    for (auto content : allocation_memory_mapping_) {
      nnapi_->ANeuralNetworksMemory_free(content.second);
    }
  }

  // Translate a node into its operands
  // It assumes that the call to Validate for has been successful for
  // the operation.
  // In case of success it returns kTfLiteOk and stores in n_op_type the
  // NNAPI Operation code.
  // Returns kTfLiteError in case of failures during mapping.
  static TfLiteStatus Map(TfLiteContext* context, int builtin_code, int version,
                          int android_sdk_version,
                          const NNAPIOpMappingArgs& mapping_args,
                          ANeuralNetworksOperationType* nn_op_type);

  // Returns true if the node can be accelerated with NNAPI.
  static bool Validate(
      const TfLiteContext* context, int builtin_code, int version,
      int android_sdk_version, const TfLiteNode* node,
      bool is_accelerator_specified,
      // Collects lists of failures collected during
      // the validation of the possibility of accelerating
      // the given node
      std::vector<NNAPIValidationFailure>* map_failures = nullptr);

  // Initialize the kernel (a NN model).
  // Any NNAPI Related error causing this method to fail will have the
  // associated error number stored in nnapi_errno
  TfLiteStatus Init(TfLiteContext* context, const TfLiteDelegateParams* params,
                    int* nnapi_errno);

  // Any NNAPI Related error causing this method to fail will have the
  // associated error number stored in nnapi_errno
  TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node,
                       int* nnapi_errno);

  // Any NNAPI Related error causing this method to fail will have the
  // associated error number stored in nnapi_errno
  TfLiteStatus Invoke(TfLiteContext* context, TfLiteNode* node,
                      int* nnapi_errno);

 private:
  // Access to NNApi.
  const NnApi* nnapi_;
  // ANN device handle.
  ANeuralNetworksDevice* nnapi_device_ = nullptr;
  // ANN API state.
  std::unique_ptr<ANeuralNetworksModel, NNFreeModel> nn_model_;
  std::unique_ptr<ANeuralNetworksCompilation, NNFreeCompilation>
      nn_compilation_;
  // Node indices that this delegate is responsible for. Indices here
  // indexes into the nodes array in the TfLiteContext.
  std::vector<int> nodes_;
  // Track indices we use
  OperandMapping operand_mapping_;
  std::map<const MMAPAllocation*, ANeuralNetworksMemory*>
      allocation_memory_mapping_;
  // Track memory map
  const std::vector<StatefulNnApiDelegate::MemoryRegistration>*
      tensor_memory_map_;
  std::vector<int> model_state_outputs_;
  std::vector<int> model_state_tfl_inputs_;
  // This is the equivalent of the pair model_state_outputs_,
  // model_state_tfl_inputs_ for all tensors where we have to keep the output
  // data available for TFLite model users
  std::vector<std::tuple<int, int>> feedback_loops_;

  std::unique_ptr<NNMemory> nn_input_memory_;
  std::unique_ptr<NNMemory> nn_output_memory_;

  void AddDequantizeOperatorsWhereNeeded(const TfLiteContext* context,
                                         int builtin_code,
                                         const TfLiteNode* node,
                                         NNAPIOpBuilder* builder,
                                         int* nnapi_errno);

  TfLiteStatus AddOpsAndTensors(TfLiteContext* context, int* nnapi_errno);

  TfLiteStatus BuildGraph(TfLiteContext* context,
                          const TfLiteIntArray* input_tensors,
                          const TfLiteIntArray* output_tensors,
                          int* nnapi_errno);
};

}  // namespace nnapi
}  // namespace delegate
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_NNAPI_NNAPI_DELEGATE_KERNEL_H_
