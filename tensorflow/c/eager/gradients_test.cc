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

#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "tensorflow/c/eager/abstract_tensor_handle.h"
#include "tensorflow/c/eager/c_api_experimental.h"
#include "tensorflow/c/eager/c_api_test_util.h"
#include "tensorflow/c/eager/c_api_unified_experimental.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/eager/gradients_internal.h"
#include "tensorflow/c/experimental/gradients/array_grad.h"
#include "tensorflow/c/experimental/gradients/math_grad.h"
#include "tensorflow/c/experimental/ops/array_ops.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/c/tf_tensor.h"
#include "tensorflow/core/lib/llvm_rtti/llvm_rtti.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/test.h"


namespace tensorflow {
namespace gradients {
namespace internal {
namespace {
using std::vector;
using tracing::TracingOperation;

class CppGradients
    : public ::testing::TestWithParam<std::tuple<const char*, bool, bool>> {
 protected:
  void SetUp() override {
    TF_SetTracingImplementation(std::get<0>(GetParam()));
  }
};

Status RegisterGradients(GradientRegistry* registry) {
  TF_RETURN_IF_ERROR(registry->Register("Add", AddRegisterer));
  TF_RETURN_IF_ERROR(registry->Register("Exp", ExpRegisterer));
  TF_RETURN_IF_ERROR(registry->Register("IdentityN", IdentityNRegisterer));
  return Status::OK();
}

// Computes `inputs[0] + inputs[1]` and records it on the tape.
Status Add(AbstractContext* ctx, Tape* tape,
           absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs,
           const GradientRegistry& registry) {
  AbstractOperationPtr add_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(
      Reset(add_op.get(), "Add", /*raw_device_name=*/nullptr, &forward_op));
  if (isa<TracingOperation>(add_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<TracingOperation>(add_op.get())->SetOpName("my_add"));
  }
  TF_RETURN_IF_ERROR(AddInput(add_op.get(), inputs[0], &forward_op));
  TF_RETURN_IF_ERROR(AddInput(add_op.get(), inputs[1], &forward_op));
  int num_retvals = 1;
  return Execute(add_op.get(), ctx, outputs, &num_retvals, &forward_op, tape,
                 registry);
}

// Computes `exp(inputs[0])` and records it on the tape.
Status Exp(AbstractContext* ctx, Tape* tape,
           absl::Span<AbstractTensorHandle* const> inputs,
           absl::Span<AbstractTensorHandle*> outputs,
           const GradientRegistry& registry) {
  AbstractOperationPtr exp_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(
      Reset(exp_op.get(), "Exp", /*raw_device_name=*/nullptr, &forward_op));
  if (isa<TracingOperation>(exp_op.get())) {
    TF_RETURN_IF_ERROR(
        dyn_cast<TracingOperation>(exp_op.get())->SetOpName("my_exp"));
  }
  TF_RETURN_IF_ERROR(AddInput(exp_op.get(), inputs[0], &forward_op));
  int num_retvals = 1;
  return Execute(exp_op.get(), ctx, outputs, &num_retvals, &forward_op, tape,
                 registry);
}

// Computes `IdentityN(inputs)` and records it on the tape.
Status IdentityN(AbstractContext* ctx, Tape* tape,
                 absl::Span<AbstractTensorHandle* const> inputs,
                 absl::Span<AbstractTensorHandle*> outputs,
                 const GradientRegistry& registry) {
  AbstractOperationPtr identity_n_op(ctx->CreateOperation());
  ForwardOperation forward_op;
  forward_op.ctx = ctx;
  TF_RETURN_IF_ERROR(Reset(identity_n_op.get(), "IdentityN",
                           /*raw_device_name=*/nullptr, &forward_op));
  if (isa<TracingOperation>(identity_n_op.get())) {
    TF_RETURN_IF_ERROR(dyn_cast<TracingOperation>(identity_n_op.get())
                           ->SetOpName("my_identity_n"));
  }
  TF_RETURN_IF_ERROR(AddInputList(identity_n_op.get(), inputs, &forward_op));
  int num_retvals = outputs.size();
  return Execute(identity_n_op.get(), ctx, outputs, &num_retvals, &forward_op,
                 tape, registry);
}

// Computes
// y = inputs[0] + inputs[1]
// return grad(y, {inputs[0], inputs[1]})
Status AddGradModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(inputs[0]));  // Watch x.
  tape->Watch(ToId(inputs[1]));  // Watch y.
  std::vector<AbstractTensorHandle*> add_outputs(1);
  TF_RETURN_IF_ERROR(Add(ctx, tape, inputs, absl::MakeSpan(add_outputs),
                         registry));  // Compute x+y.
  std::unordered_map<tensorflow::int64, TapeTensor>
      source_tensors_that_are_targets;

  std::vector<AbstractTensorHandle*> out_grads;
  TF_RETURN_IF_ERROR(tape->ComputeGradient(
      vspace, /*target_tensor_ids=*/{ToId(add_outputs[0])},
      /*source_tensor_ids=*/{ToId(inputs[0]), ToId(inputs[1])},
      source_tensors_that_are_targets,
      /*output_gradients=*/{}, &out_grads,
      /*build_default_zeros_grads=*/false));
  for (auto add_output : add_outputs) {
    add_output->Unref();
  }
  outputs[0] = out_grads[0];
  outputs[1] = out_grads[1];
  delete tape;
  return Status::OK();
}

// Computes
// y = exp(inputs[0])
// return grad(y, {inputs[0]})
Status ExpGradModel(AbstractContext* ctx,
                    absl::Span<AbstractTensorHandle* const> inputs,
                    absl::Span<AbstractTensorHandle*> outputs,
                    const GradientRegistry& registry) {
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(inputs[0]));  // Watch x.
  std::vector<AbstractTensorHandle*> exp_outputs(1);
  TF_RETURN_IF_ERROR(Exp(ctx, tape, inputs, absl::MakeSpan(exp_outputs),
                         registry));  // Compute x+y.
  std::unordered_map<tensorflow::int64, TapeTensor>
      source_tensors_that_are_targets;

  std::vector<AbstractTensorHandle*> out_grads;
  TF_RETURN_IF_ERROR(tape->ComputeGradient(
      vspace, /*target_tensor_ids=*/{ToId(exp_outputs[0])},
      /*source_tensor_ids=*/{ToId(inputs[0])}, source_tensors_that_are_targets,
      /*output_gradients=*/{}, &out_grads,
      /*build_default_zeros_grads=*/false));
  for (auto exp_output : exp_outputs) {
    exp_output->Unref();
  }
  outputs[0] = out_grads[0];
  delete tape;
  return Status::OK();
}

// Computes
// ignored, y = IdentityN(inputs[0], inputs[1])
// return grad(y, {inputs[0], inputs[1]})
// This should return [nullptr, 1].
Status IdentityNGradModel(AbstractContext* ctx,
                          absl::Span<AbstractTensorHandle* const> inputs,
                          absl::Span<AbstractTensorHandle*> outputs,
                          const GradientRegistry& registry) {
  TapeVSpace vspace(ctx);
  auto tape = new Tape(/*persistent=*/false);
  tape->Watch(ToId(inputs[0]));
  tape->Watch(ToId(inputs[1]));

  vector<AbstractTensorHandle*> identity_n_outputs(2);
  TF_RETURN_IF_ERROR(IdentityN(ctx, tape, inputs,
                               absl::MakeSpan(identity_n_outputs), registry));

  std::unordered_map<tensorflow::int64, TapeTensor>
      source_tensors_that_are_targets;
  vector<AbstractTensorHandle*> out_grads;
  TF_RETURN_IF_ERROR(tape->ComputeGradient(
      vspace, /*target_tensor_ids=*/{ToId(identity_n_outputs[1])},
      /*source_tensor_ids=*/{ToId(inputs[0]), ToId(inputs[1])},
      source_tensors_that_are_targets,
      /*output_gradients=*/{}, &out_grads,
      /*build_default_zeros_grads=*/false));
  for (auto identity_n_output : identity_n_outputs) {
    identity_n_output->Unref();
  }
  outputs[0] = out_grads[0];
  outputs[1] = out_grads[1];
  delete tape;
  return Status::OK();
}

AbstractContext* BuildFunction(const char* fn_name) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TF_ExecutionContext* graph_ctx = TF_CreateFunction(fn_name, status.get());
  return unwrap(graph_ctx);
}

Status CreateParamsForInputs(AbstractContext* ctx,
                             absl::Span<AbstractTensorHandle* const> inputs,
                             std::vector<AbstractTensorHandle*>* params) {
  tracing::TracingTensorHandle* handle = nullptr;
  for (auto input : inputs) {
    TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingContext>(ctx)->AddParameter(
        input->DataType(), &handle));
    params->emplace_back(handle);
  }
  return Status::OK();
}

using Model = std::function<Status(
    AbstractContext*, absl::Span<AbstractTensorHandle* const>,
    absl::Span<AbstractTensorHandle*>, const GradientRegistry&)>;

// Runs `model` maybe wrapped in a function.
Status RunModel(Model model, AbstractContext* ctx,
                absl::Span<AbstractTensorHandle* const> inputs,
                absl::Span<AbstractTensorHandle*> outputs, bool use_function,
                const GradientRegistry& registry) {
  if (use_function) {
    const char* fn_name = "test_fn";
    std::unique_ptr<AbstractFunction> scoped_func;
    // Returning null tensors from a tf.function is not supported, so we keep
    // track of indices in the model's outputs are nullptr in this set.
    // The FunctionDef only outputs the non-null tensors. We later pad the
    // function op outputs to have nullptrs at the `null_indices`.
    absl::flat_hash_set<int> null_indices;
    {
      AbstractContextPtr func_ctx(BuildFunction(fn_name));
      std::vector<AbstractTensorHandle*> func_inputs;
      func_inputs.reserve(inputs.size());
      TF_RETURN_IF_ERROR(
          CreateParamsForInputs(func_ctx.get(), inputs, &func_inputs));
      vector<AbstractTensorHandle*> model_outputs;
      model_outputs.resize(outputs.size());
      TF_RETURN_IF_ERROR(model(func_ctx.get(), absl::MakeSpan(func_inputs),
                               absl::MakeSpan(model_outputs), registry));
      for (auto func_input : func_inputs) {
        func_input->Unref();
      }
      AbstractFunction* func = nullptr;
      OutputList output_list;
      output_list.expected_num_outputs = 0;
      output_list.outputs.reserve(outputs.size());
      for (int i = 0; i < model_outputs.size(); i++) {
        if (model_outputs[i]) {
          output_list.outputs.emplace_back(model_outputs[i]);
          output_list.expected_num_outputs += 1;
        } else {
          null_indices.insert(i);
        }
      }
      TF_RETURN_IF_ERROR(dyn_cast<tracing::TracingContext>(func_ctx.get())
                             ->Finalize(&output_list, &func));
      scoped_func.reset(func);
      for (auto output : output_list.outputs) {
        output->Unref();
      }
      TF_RETURN_IF_ERROR(ctx->RegisterFunction(func));
    }

    AbstractOperationPtr fn_op(ctx->CreateOperation());
    TF_RETURN_IF_ERROR(fn_op->Reset(fn_name, /*raw_device_name=*/nullptr));
    for (auto input : inputs) {
      TF_RETURN_IF_ERROR(fn_op->AddInput(input));
    }
    int retvals = outputs.size() - null_indices.size();
    vector<AbstractTensorHandle*> fn_outputs(retvals);
    TF_RETURN_IF_ERROR(fn_op->Execute(
        absl::Span<AbstractTensorHandle*>(fn_outputs.data(), fn_outputs.size()),
        &retvals));
    int skipped_indices = 0;
    for (int i = 0; i < outputs.size(); i++) {
      if (!null_indices.contains(i)) {
        outputs[i] = fn_outputs[i - skipped_indices];
      } else {
        skipped_indices += 1;
      }
    }
    TF_RETURN_IF_ERROR(ctx->RemoveFunction(fn_name));
    return Status::OK();
  } else {
    return model(ctx, inputs, outputs, registry);
  }
}

Status BuildImmediateExecutionContext(bool use_tfrt, AbstractContext** ctx) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_ContextOptions* opts = TFE_NewContextOptions();
  TFE_ContextOptionsSetTfrt(opts, use_tfrt);
  *ctx = unwrap(TF_NewEagerExecutionContext(opts, status.get()));
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  TFE_DeleteContextOptions(opts);
  return Status::OK();
}

Status TestScalarTensorHandle(AbstractContext* ctx, float value,
                              AbstractTensorHandle** tensor) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_Context* eager_ctx =
      TF_ExecutionContextGetTFEContext(wrap(ctx), status.get());
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  TFE_TensorHandle* input_eager = TestScalarTensorHandle(eager_ctx, value);
  *tensor =
      unwrap(TF_CreateAbstractTensorFromEagerTensor(input_eager, status.get()));
  return Status::OK();
}

Status getValue(AbstractTensorHandle* t, TF_Tensor** result_tensor) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  TFE_TensorHandle* result_t =
      TF_AbstractTensorGetEagerTensor(wrap(t), status.get());
  TF_RETURN_IF_ERROR(StatusFromTF_Status(status.get()));
  *result_tensor = TFE_TensorHandleResolve(result_t, status.get());
  return Status::OK();
}

TEST_P(CppGradients, TestAddGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  AbstractTensorHandlePtr x;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 2.0f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    x.reset(x_raw);
  }

  AbstractTensorHandlePtr y;
  {
    AbstractTensorHandle* y_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 2.0f, &y_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    y.reset(y_raw);
  }

  GradientRegistry registry;
  Status s = RegisterGradientAdd(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Pseudo-code:
  //
  // tape.watch(x)
  // tape.watch(y)
  // y = x + y
  // outputs = tape.gradient(y, [x, y])
  std::vector<AbstractTensorHandle*> outputs(2);
  s = RunModel(AddGradModel, ctx.get(), {x.get(), y.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* result_tensor;
  s = getValue(outputs[0], &result_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  auto result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_EQ(*result_value, 1.0);
  outputs[0]->Unref();
  TF_DeleteTensor(result_tensor);
  result_tensor = nullptr;

  s = getValue(outputs[1], &result_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_EQ(*result_value, 1.0);
  outputs[1]->Unref();
  TF_DeleteTensor(result_tensor);
}

TEST_P(CppGradients, TestExpGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  AbstractTensorHandlePtr x;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 1.0f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    x.reset(x_raw);
  }

  GradientRegistry registry;
  Status s = RegisterGradients(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  // Pseudo-code:
  //
  // tape.watch(x)
  // y = exp(x)
  // outputs = tape.gradient(y, x)
  std::vector<AbstractTensorHandle*> outputs(1);
  s = RunModel(ExpGradModel, ctx.get(), {x.get()}, absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  TF_Tensor* result_tensor;
  s = getValue(outputs[0], &result_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  auto result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_NEAR(*result_value, 2.718, 0.001);
  outputs[0]->Unref();
  TF_DeleteTensor(result_tensor);
  result_tensor = nullptr;
}

TEST_P(CppGradients, TestIdentityNGrad) {
  // Pseudo-code:
  //
  // tape.watch(x1)
  // tape.watch(x2)
  // unused, y = IdentityN([x1, x2])
  // outputs = tape.gradient(y, [x1, x2])
  // Expected: [nullptr, 1]
  //
  // This test is interesting because the current implementation of GradientTape
  // would return [0, 1] whereas we use build_default_zeros_grads=false here
  // so we get back [nullptr, 1].
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  AbstractTensorHandlePtr x1;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 1.0f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    x1.reset(x_raw);
  }
  AbstractTensorHandlePtr x2;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 1.0f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    x2.reset(x_raw);
  }

  GradientRegistry registry;
  Status s = RegisterGradients(&registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  std::vector<AbstractTensorHandle*> outputs(2);
  s = RunModel(IdentityNGradModel, ctx.get(), {x1.get(), x2.get()},
               absl::MakeSpan(outputs),
               /*use_function=*/!std::get<2>(GetParam()), registry);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  EXPECT_EQ(outputs[0], nullptr);
  TF_Tensor* result_tensor;
  s = getValue(outputs[1], &result_tensor);
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  auto result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_EQ(*result_value, 1.0);
  outputs[1]->Unref();
  TF_DeleteTensor(result_tensor);
  result_tensor = nullptr;
}

// TODO(b/164171226): Enable this test with tfrt after AddInputList is
// supported. It is needed for IdentityN.
#ifdef PLATFORM_GOOGLE
INSTANTIATE_TEST_SUITE_P(
    UnifiedCAPI, CppGradients,
    ::testing::Combine(::testing::Values("graphdef", "mlir"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));
#else
INSTANTIATE_TEST_SUITE_P(
    UnifiedCAPI, CppGradients,
    ::testing::Combine(::testing::Values("graphdef", "mlir"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));
#endif
}  // namespace
}  // namespace internal
}  // namespace gradients
}  // namespace tensorflow
