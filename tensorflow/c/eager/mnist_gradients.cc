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
#include "tensorflow/c/eager/gradients.h"

#include <memory>

#include "absl/types/span.h"
#include "tensorflow/c/eager/abstract_tensor_handle.h"
#include "tensorflow/c/eager/c_api_experimental.h"
#include "tensorflow/c/eager/c_api_test_util.h"
#include "tensorflow/c/eager/c_api_unified_experimental.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/eager/gradients_internal.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/c/tf_tensor.h"
#include "tensorflow/core/lib/llvm_rtti/llvm_rtti.h"
// #include "tensorflow/core/platform/errors.h"
// #include "tensorflow/core/platform/test.h"


namespace tensorflow {
namespace gradients {
namespace internal {
namespace {

// =================== Register gradients for Add ============================
class AddGradientFunction : public GradientFunction {
 public:
  explicit AddGradientFunction(AbstractContext* ctx) : ctx_(ctx) {}
  
  Status Compute(absl::Span<AbstractTensorHandle* const> grad_inputs,
                 std::vector<AbstractTensorHandle*>* grad_outputs) override {
    
    grad_outputs->resize(2);
    std::vector<AbstractTensorHandle*> identity_outputs(1);
    TF_RETURN_IF_ERROR(Identity(ctx_, {grad_inputs[0]},
                                absl::MakeSpan(identity_outputs), "Id0"));
    (*grad_outputs)[0] = identity_outputs[0];
    TF_RETURN_IF_ERROR(Identity(ctx_, {grad_inputs[0]},
                                absl::MakeSpan(identity_outputs), "Id1"));
    (*grad_outputs)[1] = identity_outputs[0];
    return Status::OK();
  }
  ~AddGradientFunction() override {}

 private:
  AbstractContext* ctx_;
};

GradientFunction* AddRegisterer(const ForwardOperation& op) {
  return new AddGradientFunction(op.ctx);
}
 
Status RegisterGradientAdd(GradientRegistry* registry) {
  return registry->Register("Add", AddRegisterer);
}

// =================== Register gradients for MatMul ============================
class MatMulGradientFunction : public GradientFunction {
 public:
  explicit MatMulGradientFunction(AbstractContext* ctx, std::vector<AbstractTensorHandle*> f_inputs) : 
            ctx_(ctx), forward_inputs(f_inputs) {}
  
  Status Compute(absl::Span<AbstractTensorHandle* const> grad_inputs,
                 std::vector<AbstractTensorHandle*>* grad_outputs) override {
    
    /* Given upstream grad U and a matmul op A*B, the gradients are:
     *      
     *    dA = U * B.T   
     *    dB = A.T * U
     *
     *    where A.T means `transpose(A)`
     */

    AbstractTensorHandle* upstream_grad = grad_inputs[0];
    grad_outputs->resize(2);
    std::vector<AbstractTensorHandle*> matmul_outputs(1);

    // Gradient for A
    TF_RETURN_IF_ERROR(MatMul(ctx_, {upstream_grad, forward_inputs[1]},
                              absl::MakeSpan(matmul_outputs), "mm0",  
                              /*transpose_a = */false, /*transpose_b = */true));

    (*grad_outputs)[0] = matmul_outputs[0];

    // Gradient for B
    TF_RETURN_IF_ERROR(MatMul(ctx_, {upstream_grad},
                              absl::MakeSpan(matmul_outputs), "mm1", 
                              /*transpose_a = */true, /*transpose_b = */false));

    (*grad_outputs)[1] = matmul_outputs[0];
    return Status::OK();
  }
  ~MatMulGradientFunction() override {}

 private:
  AbstractContext* ctx_;
  std::vector<AbstractTensorHandle*> forward_inputs;

};

GradientFunction* MatMulRegisterer(const ForwardOperation& op) {
  return new MatMulGradientFunction(op.ctx, op.inputs);
}
 
Status RegisterGradientMatMul(GradientRegistry* registry) {
  return registry->Register("MatMul", MatMulRegisterer);
}
// =================== End gradient registrations ============================

}  // namespace
}  // namespace internal
}  // namespace gradients
}  // namespace tensorflow

