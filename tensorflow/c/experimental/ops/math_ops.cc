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
#include "tensorflow/c/experimental/ops/math_ops.h"

#include "tensorflow/c/eager/abstract_context.h"
#include "tensorflow/c/eager/abstract_tensor_handle.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/experimental/ops/array_ops.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/platform/errors.h"
namespace tensorflow {
namespace ops {
using tensorflow::tracing::TracingOperation;

Status Mul(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr mul_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(mul_op->Reset("Mul", /*raw_device_name=*/nullptr));
  if (isa<TracingOperation>(mul_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<TracingOperation>(mul_op.get())->SetOpName(name));
  }
  TF_RETURN_IF_ERROR(mul_op->AddInput(inputs[0]));
  TF_RETURN_IF_ERROR(mul_op->AddInput(inputs[1]));
  int num_retvals = 1;
  return mul_op->Execute(outputs, &num_retvals);
}

Status Conj(AbstractContext* ctx,
            absl::Span<AbstractTensorHandle* const> inputs,
            absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  auto dtype = inputs[0]->DataType();
  if (DataTypeIsFloating(BaseType(dtype)) ||
      DataTypeIsInteger(BaseType(dtype))) {
    TF_RETURN_IF_ERROR(Identity(ctx, inputs, outputs, name));
  } else {
    return errors::Unimplemented("Conj does not support complex types yet.");
  }
  return Status::OK();
}

Status Add(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr add_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(add_op->Reset("AddV2", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(add_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(add_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(add_op->AddInput(inputs[0]));
  TF_RETURN_IF_ERROR(add_op->AddInput(inputs[1]));

  int num_retvals = 1;
  TF_RETURN_IF_ERROR(add_op->Execute(outputs, &num_retvals));
  return Status::OK();
}

Status Sub(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr sub_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(sub_op->Reset("Sub", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(sub_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(sub_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(sub_op->AddInput(inputs[0]));
  TF_RETURN_IF_ERROR(sub_op->AddInput(inputs[1]));

  int num_retvals = 1;
  TF_RETURN_IF_ERROR(sub_op->Execute(outputs, &num_retvals));
  return Status::OK();
}


Status MatMul(AbstractContext* ctx,
              absl::Span<AbstractTensorHandle* const> inputs,
              absl::Span<AbstractTensorHandle*> outputs, const char* name,
              bool transpose_a = false, bool transpose_b = false) {
  AbstractOperationPtr matmul_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(matmul_op->Reset("MatMul", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(matmul_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(matmul_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(matmul_op->AddInput(inputs[0]));
  TF_RETURN_IF_ERROR(matmul_op->AddInput(inputs[1]));

  TF_RETURN_IF_ERROR(matmul_op->SetAttrBool("transpose_a", transpose_a));
  TF_RETURN_IF_ERROR(matmul_op->SetAttrBool("transpose_b", transpose_b));

  int num_retvals = 1;
  TF_RETURN_IF_ERROR(matmul_op->Execute(outputs, &num_retvals));
  return Status::OK();
}

Status Neg(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr neg_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(neg_op->Reset("Neg", /*raw_device_name=*/nullptr));
  if (isa<TracingOperation>(neg_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<TracingOperation>(neg_op.get())->SetOpName(name));
  }
  TF_RETURN_IF_ERROR(neg_op->AddInput(inputs[0]));

  int num_retvals = 1;
  return neg_op->Execute(outputs, &num_retvals);
}

Status Prod(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr prod_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(prod_op->Reset("Prod", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(prod_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(prod_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(prod_op->AddInput(inputs[0])); // input_vals
  TF_RETURN_IF_ERROR(prod_op->AddInput(inputs[1])); // reduction_indices

  int num_retvals = 1;
  TF_RETURN_IF_ERROR(prod_op->Execute(outputs, &num_retvals));
  return Status::OK();
}

Status Sum(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr sum_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(sum_op->Reset("Sum", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(sum_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(sum_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(sum_op->AddInput(inputs[0])); // input_vals
  TF_RETURN_IF_ERROR(sum_op->AddInput(inputs[1])); // reduction_indices

  int num_retvals = 1;
  TF_RETURN_IF_ERROR(sum_op->Execute(outputs, &num_retvals));
  return Status::OK();
}

Status EuclideanNorm(AbstractContext* ctx, absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs, const char* name) {
  AbstractOperationPtr norm_op(ctx->CreateOperation());
  TF_RETURN_IF_ERROR(norm_op->Reset("EuclideanNorm", /*raw_device_name=*/nullptr));

  if (isa<tracing::TracingOperation>(norm_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<tracing::TracingOperation>(norm_op.get())->SetOpName(name));
  }

  TF_RETURN_IF_ERROR(norm_op->AddInput(inputs[0])); // input_vals
  TF_RETURN_IF_ERROR(norm_op->AddInput(inputs[1])); // reduction_indices

  int num_retvals = 1;
  TF_RETURN_IF_ERROR(norm_op->Execute(outputs, &num_retvals));
  return Status::OK();
}

}  // namespace ops
}  // namespace tensorflow
