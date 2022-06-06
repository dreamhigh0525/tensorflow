/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_DELEGATES_GPU_COMMON_MODEL_BUILDER_INTERNAL_H_
#define TENSORFLOW_LITE_DELEGATES_GPU_COMMON_MODEL_BUILDER_INTERNAL_H_

#include "absl/container/flat_hash_set.h"
#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/delegates/gpu/common/operation_parser.h"

namespace tflite {
namespace gpu {

// Returns a new TFLiteOperationParser object which parses the TFLite operator
// in the given TfLiteRegistration object.
std::unique_ptr<TFLiteOperationParser> NewOperationParser(
    const TfLiteRegistration* registration, bool allow_quant_ops = false,
    const absl::flat_hash_set<TfLiteBuiltinOperator>* excluded_ops = nullptr);

}  // namespace gpu
}  // namespace tflite

#endif  // TENSORFLOW_LITE_DELEGATES_GPU_COMMON_MODEL_BUILDER_INTERNAL_H_
