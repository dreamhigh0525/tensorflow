/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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
/// \file
/// Defines tflite::Interpreter and tflite::InterpreterBuilder.
///
#ifndef TENSORFLOW_LITE_MODEL_H_
#define TENSORFLOW_LITE_MODEL_H_

#include "tensorflow/lite/model_builder.h"

#if TFLITE_EXPERIMENTAL_RUNTIME_EAGER
#include "tensorflow/lite/experimental/tf_runtime/lib/eager_model.h"
#else
#include "tensorflow/lite/interpreter_builder.h"
#endif

namespace tflite {

#if TFLITE_EXPERIMENTAL_RUNTIME_EAGER
using InterpreterBuilder = tflrt::EagerTfLiteInterpreterBuilderAPI;
using Interpreter = tflrt::EagerInterpreter;
#else
using InterpreterBuilder = impl::InterpreterBuilder;
#endif

}  // namespace tflite

#endif  // TENSORFLOW_LITE_MODEL_H_
