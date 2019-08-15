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
#include "tensorflow/lite/c/c_api_internal.h"
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

// The kernel that represents the node sub set of TF Lite being run on NN API.
class NNAPIDelegateKernel {
 public:
  NNAPIDelegateKernel() { nnapi_ = NnApiImplementation(); }
  ~NNAPIDelegateKernel() {
    for (auto content : allocation_memory_mapping_) {
      nnapi_->ANeuralNetworksMemory_free(content.second);
    }
  }

  typedef ANeuralNetworksOperationType (*MappingFn)(
      const NNAPIOpMappingArgs& mapping_args);

  // Return a function that knows how to translate a node into its operands
  // when called. You can use this function to see if a node is supported
  // (i.e. if the returned MappingFn is null, then the node is not supported).
  static MappingFn Map(const TfLiteContext* context, int builtin_code,
                       int version, int android_sdk_version,
                       const TfLiteNode* node, bool is_accelerator_specified);

  // Initialize the kernel (a NN model).
  TfLiteStatus Init(TfLiteContext* context, const TfLiteDelegateParams* params);

  TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node);

  TfLiteStatus Invoke(TfLiteContext* context, TfLiteNode* node);

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
                                         NNAPIOpBuilder* builder);

  TfLiteStatus AddOpsAndTensors(TfLiteContext* context);

  TfLiteStatus BuildGraph(TfLiteContext* context,
                          const TfLiteIntArray* input_tensors,
                          const TfLiteIntArray* output_tensors);
};

}  // namespace nnapi
}  // namespace delegate
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_NNAPI_NNAPI_DELEGATE_KERNEL_H_
