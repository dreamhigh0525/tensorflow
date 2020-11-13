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
#ifndef TENSORFLOW_C_EAGER_UNIFIED_API_TESTUTIL_H_
#define TENSORFLOW_C_EAGER_UNIFIED_API_TESTUTIL_H_

#include "tensorflow/c/eager/abstract_context.h"
#include "tensorflow/c/eager/abstract_tensor_handle.h"
#include "tensorflow/core/platform/status.h"

namespace tensorflow {

// Builds and returns a `TracingContext` using the default tracing impl.
AbstractContext* BuildFunction(const char* fn_name);

// Creates parameters (placeholders) in the tracing `ctx` using the shape and
// dtype of `inputs`.
Status CreateParamsForInputs(AbstractContext* ctx,
                             absl::Span<AbstractTensorHandle* const> inputs,
                             std::vector<AbstractTensorHandle*>* params);

// A callable that takes tensor inputs and returns zero or more tensor outputs.
using Model = std::function<Status(AbstractContext*,
                                   absl::Span<AbstractTensorHandle* const>,
                                   absl::Span<AbstractTensorHandle*>)>;

// Runs `model` maybe wrapped in a function call op. This can be thought as
// being equivalent to the following python code.
//
// if use_function:
//   outputs = tf.function(model)(inputs)
// else:
//   outputs = model(inputs)
Status RunModel(Model model, AbstractContext* ctx,
                absl::Span<AbstractTensorHandle* const> inputs,
                absl::Span<AbstractTensorHandle*> outputs, bool use_function);

Status BuildImmediateExecutionContext(bool use_tfrt, AbstractContext** ctx);

// Get a Scalar TensorHandle with given float value.
Status TestScalarTensorHandle(AbstractContext* ctx, float value,
                              AbstractTensorHandle** tensor);

// Get a Matrix TensorHandle with given float values and dimensions.
Status TestTensorHandleWithDimsFloat(AbstractContext* ctx, float* data,
                                     int64* dims, int num_dims,
                                     AbstractTensorHandle** tensor);
}  // namespace tensorflow

#endif  // TENSORFLOW_C_EAGER_UNIFIED_API_TESTUTIL_H_
