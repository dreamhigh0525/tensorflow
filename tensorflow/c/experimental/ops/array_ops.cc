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
#include "tensorflow/c/experimental/ops/array_ops.h"

#include "tensorflow/core/platform/errors.h"

namespace tensorflow {
namespace ops {
// Creates an Identity op.
Status Identity(AbstractContext* ctx,
                absl::Span<AbstractTensorHandle* const> inputs,
                absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr identity_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(
      identity_op->Reset("Identity", /*raw_device_name=*/nullptr));
  if (isa<tensorflow::tracing::TracingOperation>(identity_op.get())) {
    TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingOperation>(identity_op.get())
                           ->SetOpName(name));
  }
  TF_RETURN_IF_ERROR(identity_op->AddInput(inputs[0]));
  int num_retvals = 1;
  return identity_op->Execute(outputs, &num_retvals);
}

}  // namespace ops
}  // namespace tensorflow
