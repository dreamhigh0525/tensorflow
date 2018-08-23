/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CONTRIB_LITE_DELEGATES_EAGER_TEST_UTIL_H_
#define TENSORFLOW_CONTRIB_LITE_DELEGATES_EAGER_TEST_UTIL_H_

#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/contrib/lite/kernels/test_util.h"

namespace tflite {
namespace eager {
namespace testing {

enum TfOpType {
  kUnpack,
  kIdentity,
  kAdd,
  kMul,
  // Represents an op that does not exist in TensorFlow.
  kNonExistent,
  // Represents an valid TensorFlow op where the NodeDef is incompatible.
  kIncompatibleNodeDef,
};

// This class creates models with TF and TFLite ops. In order to use this class
// to test the Eager delegate, implement a function that calls
// interpreter->ModifyGraphWithDelegate.
class EagerModelTest : public ::testing::Test {
 public:
  EagerModelTest() {}
  ~EagerModelTest() {}

  bool Invoke();

  // Sets the tensor's values at the given index.
  void SetValues(int tensor_index, const std::vector<float>& values);

  // Returns the tensor's values at the given index.
  std::vector<float> GetValues(int tensor_index);

  // Sets the tensor's shape at the given index.
  void SetShape(int tensor_index, const std::vector<int>& values);

  // Returns the tensor's shape at the given index.
  std::vector<int> GetShape(int tensor_index);

  const TestErrorReporter& error_reporter() const { return error_reporter_; }

  // Adds `num_tensor` tensors to the model. `inputs` contains the indices of
  // the input tensors and `outputs` contains the indices of the output
  // tensors. All tensors are set to have `type` and `dims`.
  void AddTensors(int num_tensors, const std::vector<int>& inputs,
                  const std::vector<int>& outputs, const TfLiteType& type,
                  const std::vector<int>& dims);

  // Adds a TFLite Mul op. `inputs` contains the indices of the input tensors
  // and `outputs` contains the indices of the output tensors.
  void AddTfLiteMulOp(const std::vector<int>& inputs,
                      const std::vector<int>& outputs);

  // Adds a TensorFlow op. `inputs` contains the indices of the
  // input tensors and `outputs` contains the indices of the output tensors.
  // This function is limited to the set of ops defined in TfOpType.
  void AddTfOp(TfOpType op, const std::vector<int>& inputs,
               const std::vector<int>& outputs);

 protected:
  std::unique_ptr<Interpreter> interpreter_;
  TestErrorReporter error_reporter_;

 private:
  // Helper method to add a TensorFlow op. tflite_names needs to start with
  // "Eager" in order to work with the Eager delegate.
  void AddTfOp(const char* tflite_name, const string& tf_name,
               const string& nodedef_str, const std::vector<int>& inputs,
               const std::vector<int>& outputs);

  std::vector<std::vector<uint8_t>> flexbuffers_;
};

}  // namespace testing
}  // namespace eager
}  // namespace tflite

#endif  // TENSORFLOW_CONTRIB_LITE_DELEGATES_EAGER_TEST_UTIL_H_
