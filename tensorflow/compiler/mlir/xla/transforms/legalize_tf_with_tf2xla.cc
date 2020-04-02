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
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "llvm/ADT/Optional.h"
#include "mlir/IR/Diagnostics.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/Module.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/op_or_arg_name_mapper.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h.inc"
#include "tensorflow/compiler/mlir/tensorflow/translate/export_tf_dialect_op.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_type.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/translate_utils.h"
#include "tensorflow/compiler/mlir/xla/ir/mlir_hlo_builder.h"
#include "tensorflow/compiler/tf2xla/xla_compilation_device.h"
#include "tensorflow/compiler/tf2xla/xla_context.h"
#include "tensorflow/compiler/tf2xla/xla_expression.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/process_function_library_runtime.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/node_properties.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/stream_executor/lib/statusor.h"
#include "tensorflow/stream_executor/stream_executor.h"

namespace mlir {
namespace xla_hlo {
namespace {

template <typename T, size_t N>
using InlinedVector = tensorflow::gtl::InlinedVector<T, N>;  // non-absl ok

static bool IsOpWhitelisted(Operation* op) {
  // White-listed TensorFlow ops are known to have well behaved tf2xla kernels
  // building valid MLIR using MlirHloBuilder.
  // TODO(hinsu): Drop explicit whitelist when MLIR based bridge is enabled for
  // all tf2xla kernels.
  return isa<TF::AbsOp>(op) || isa<TF::Atan2Op>(op) || isa<TF::CastOp>(op) ||
         isa<TF::InvOp>(op) || isa<TF::SelectV2Op>(op);
}

static llvm::Optional<std::string> GetExecutionDevice(
    const std::string& device_type, const Location& loc) {
  if (device_type == "XLA_CPU_JIT") return std::string("XLA_CPU");
  if (device_type == "XLA_TPU_JIT") return std::string("TPU");
  // TODO(hinsu): Support GPU device along with a test for it.

  emitError(loc) << "unsupported device for legalization with tf2xla kernels: "
                 << device_type;
  return llvm::None;
}

static std::unique_ptr<tensorflow::StaticDeviceMgr> CreateDeviceMgr(
    const std::string& device_type, const Location& loc) {
  auto device_or = GetExecutionDevice(device_type, loc);
  if (!device_or) return nullptr;

  auto* factory = tensorflow::DeviceFactory::GetFactory(*device_or);
  if (!factory) {
    emitError(loc) << "failed to create DeviceFactory for device: "
                   << device_type;
    return nullptr;
  }
  std::vector<std::unique_ptr<tensorflow::Device>> devices;
  auto status = factory->CreateDevices(
      tensorflow::SessionOptions(),
      /*name_prefix=*/"/job:localhost/replica:0/task:0", &devices);
  if (!status.ok()) {
    emitError(loc) << status.ToString();
    return nullptr;
  }

  auto device = absl::make_unique<tensorflow::XlaCompilationDevice>(
      tensorflow::SessionOptions(), tensorflow::DeviceType(device_type));
  return absl::make_unique<tensorflow::StaticDeviceMgr>(std::move(device));
}

class FuncLegalizer {
 public:
  static LogicalResult Legalize(FuncOp func, const std::string& device_type) {
    FuncLegalizer legalizer(func, device_type);
    if (failed(legalizer.PrepareParams())) return failure();
    return legalizer.Legalize();
  }

 private:
  FuncLegalizer(FuncOp func, const std::string& device_type)
      : func_(func), device_type_(device_type), hlo_builder_(func) {}

  ~FuncLegalizer() { context_->Unref(); }

  // Prepares OpKernelContext params common to all the ops.
  // Emits an error on failure.
  LogicalResult PrepareParams();

  // Tries to legalize supported TensorFlow ops.
  // Emits an error on failure.
  LogicalResult Legalize();

  // Tries to legalize the specified TensorFlow op, if supported.
  //
  // Emits an error and returns failure if an error is encountered during
  // conversion. Note that success return value doesn't mean successful
  // legalization.
  LogicalResult LegalizeOp(Operation* op);

  FuncOp func_;
  std::string device_type_;

  ::xla::MlirHloBuilder hlo_builder_;
  tensorflow::OpOrArgLocNameMapper name_mapper_;

  tensorflow::XlaContext* context_;  // Ref-counted.

  std::unique_ptr<tensorflow::StaticDeviceMgr> device_mgr_;
  tensorflow::Device* device_;  // Owned by device_mgr_;
  std::unique_ptr<tensorflow::ScopedStepContainer> step_container_;
  std::unique_ptr<tensorflow::FunctionLibraryDefinition> flib_def_;
  std::unique_ptr<tensorflow::ProcessFunctionLibraryRuntime> pflr_;
  tensorflow::OpKernelContext::Params params_;
};

LogicalResult FuncLegalizer::PrepareParams() {
  // XlaCompiler within the context is only used by the functional ops to
  // compile functions. We are not handling those at the moment so XlaCompiler
  // is not required.
  context_ = new tensorflow::XlaContext(/*compiler=*/nullptr, &hlo_builder_);
  context_->Ref();

  mlir::Location loc = func_.getLoc();
  device_mgr_ = CreateDeviceMgr(device_type_, loc);
  if (!device_mgr_) return failure();

  // Type of params_.device is DeviceBase* so store it as Device* to access
  // derived class method.
  device_ = device_mgr_->ListDevices().front();
  params_.device = device_;
  params_.resource_manager = device_->resource_manager();

  // Resources are cleared at the time of device manager destruction so pass
  // no-op cleanup function.
  auto cleanup = [](const std::string& name) {};
  // Use step_id zero as we only have a single context concurrently and
  // concurrently running each of the MLIR functions create a new device.
  step_container_ = absl::make_unique<tensorflow::ScopedStepContainer>(
      /*step_id=*/0, cleanup);
  tensorflow::Status status = step_container_->Create(
      device_->resource_manager(),
      tensorflow::XlaContext::kXlaContextResourceName, context_);
  if (!status.ok()) {
    emitError(loc) << "failed to create XlaContext resource: "
                   << status.ToString();
    return failure();
  }
  params_.step_container = step_container_.get();

  tensorflow::StatusOr<int64_t> version_or =
      tensorflow::GetTfGraphProducerVersion(
          func_.getParentOfType<mlir::ModuleOp>());
  if (!version_or.ok()) {
    emitError(loc) << version_or.status().ToString();
    return failure();
  }

  flib_def_ = absl::make_unique<tensorflow::FunctionLibraryDefinition>(
      tensorflow::OpRegistry::Global(), tensorflow::FunctionDefLibrary());
  pflr_ = absl::make_unique<tensorflow::ProcessFunctionLibraryRuntime>(
      device_mgr_.get(), tensorflow::Env::Default(), /*config=*/nullptr,
      version_or.ValueOrDie(), flib_def_.get(), tensorflow::OptimizerOptions());
  params_.function_library = pflr_->GetFLR(device_->name());
  return success();
}

LogicalResult FuncLegalizer::Legalize() {
  // TensorFlow functions don't use CFGs.
  if (func_.getBlocks().size() > 1) {
    emitError(func_.getLoc()) << "requires at most one block in a TF function";
    return failure();
  }
  if (func_.getBlocks().empty()) return success();
  Block& block = func_.getBlocks().front();

  std::vector<Operation*> ops;
  ops.reserve(block.getOperations().size());
  for (Operation& op : block.getOperations()) {
    ops.push_back(&op);
  }

  for (Operation* op : ops) {
    if (failed(LegalizeOp(op))) return failure();
  }
  return success();
}

LogicalResult FuncLegalizer::LegalizeOp(Operation* op) {
  if (!IsOpWhitelisted(op)) return success();

  // Only static shaped operands are supported in XLA builders for now.
  for (Type ty : op->getOperandTypes()) {
    auto ranked_ty = ty.cast<RankedTensorType>();
    if (!ranked_ty || !ranked_ty.hasStaticShape()) {
      op->emitRemark() << "lowering requires static shaped operands";
      return success();
    }
  }

  auto nodedef_or = tensorflow::ConvertTFDialectOpToNodeDef(
      op, name_mapper_.GetUniqueName(op), /*ignore_unregistered_attrs=*/true);
  if (!nodedef_or.ok()) {
    op->emitRemark() << "failed to convert op to NodeDef: "
                     << nodedef_or.status().ToString();
    return success();
  }

  std::shared_ptr<const tensorflow::NodeProperties> props;
  tensorflow::Status status = tensorflow::NodeProperties::CreateFromNodeDef(
      *nodedef_or.ValueOrDie(),
      params_.function_library->GetFunctionLibraryDefinition(), &props);
  if (!status.ok()) {
    op->emitRemark() << "failed to create NodeProperties: "
                     << status.ToString();
    return success();
  }
  tensorflow::OpKernel* op_kernel_raw;
  status = params_.function_library->CreateKernel(props, &op_kernel_raw);
  if (!status.ok()) {
    op->emitRemark() << "failed to create tf2xla kernel: " << status.ToString();
    return success();
  }
  // Transfer ownership of the kernel to a local smart pointer.
  auto op_kernel = absl::WrapUnique(op_kernel_raw);

  // TensorValue in inputs are backed by tensors which in turn depend on
  // expressions. So, pre-allocate them to the required size.
  InlinedVector<tensorflow::XlaExpression, 4> expressions;
  InlinedVector<tensorflow::Tensor, 4> tensors;
  InlinedVector<tensorflow::TensorValue, 4> inputs;
  expressions.reserve(op->getNumOperands());
  tensors.reserve(op->getNumOperands());
  inputs.reserve(op->getNumOperands());

  // Prepare the list of Tensor inputs for the kernel.
  for (Value operand : op->getOperands()) {
    // Skip this op if XLA doesn't support this operand type.
    auto xla_op_or = hlo_builder_.MakeXlaOp(operand);
    if (!xla_op_or.ok()) {
      op->emitRemark() << "skipping legalization due to "
                       << xla_op_or.status().ToString();
      return success();
    }
    ::xla::XlaOp xla_op = xla_op_or.ValueOrDie();

    tensorflow::DataType dtype;
    status = tensorflow::ConvertToDataType(operand.getType(), &dtype);
    if (!status.ok()) {
      op->emitRemark() << "skipping legalization due to " << status.ToString();
      return success();
    }

    auto expression = tensorflow::XlaExpression::XlaOp(xla_op, dtype);
    expressions.push_back(expression);

    if (!tensorflow::DataTypeCanUseMemcpy(dtype)) {
      op->emitRemark() << "skipping legalization due to unsupported type "
                       << operand.getType();
      return success();
    }

    auto shape_or = expression.GetShape();
    if (!shape_or.ok()) {
      op->emitRemark() << "failed to get shape for expression. "
                       << expression.HumanString();
      return success();
    }

    tensors.emplace_back(
        device_->GetAllocator(tensorflow::AllocatorAttributes()), dtype,
        shape_or.ValueOrDie());
    tensorflow::Tensor& tensor = tensors.back();
    tensorflow::XlaOpKernelContext::AssignExpressionToTensor(expression,
                                                             &tensor);
    inputs.emplace_back(&tensor);
  }

  params_.inputs = &inputs;
  params_.op_kernel = op_kernel.get();
  llvm::SmallVector<tensorflow::AllocatorAttributes, 4> output_attr(
      op->getNumResults());
  params_.output_attr_array = output_attr.data();

  hlo_builder_.setInsertionPoint(op);
  hlo_builder_.SetLocation(op->getLoc());

  // Execute the kernel.
  tensorflow::OpKernelContext op_context(&params_, op->getNumResults());
  device_->Compute(params_.op_kernel, &op_context);
  if (!op_context.status().ok()) {
    op->emitRemark() << "compilation to HLO failed: "
                     << op_context.status().ToString();
    return success();
  }

  // Replace uses of old results using the corresponding value after the
  // lowering.
  for (int i = 0, e = op->getNumResults(); i < e; i++) {
    tensorflow::Tensor* output = op_context.mutable_output(i);
    const tensorflow::XlaExpression* expr =
        tensorflow::XlaOpKernelContext::CastExpressionFromTensor(*output);
    if (expr->kind() != tensorflow::XlaExpression::Kind::kXlaOp)
      return op->emitError(
          "expects XlaExpression of kind kXlaOp in compiled output");
    auto value = hlo_builder_.GetValue(expr->handle());
    op->getResult(i).replaceAllUsesWith(value);
  }

  op->erase();
  return success();
}

class LegalizeTF : public FunctionPass<LegalizeTF> {
 public:
  LegalizeTF() = default;

  LegalizeTF(const LegalizeTF&) {}

  void runOnFunction() override {
    if (failed(FuncLegalizer::Legalize(getFunction(), device_type_)))
      signalPassFailure();
  }

 private:
  // TODO(hinsu): Support finer grained device type assignment instead of a
  // global device type for all TensorFlow ops.
  Option<std::string> device_type_{
      *this, "device-type",
      llvm::cl::desc("XLA device type for execution of TensorFlow ops. "
                     "Supports XLA_CPU_JIT and XLA_TPU_JIT for now.")};
};

static PassRegistration<LegalizeTF> pass(
    "xla-legalize-tf-with-tf2xla",
    "Legalize from TensorFlow to the HLO dialect using tf2xla kernels");

}  // end namespace

}  // end namespace xla_hlo
}  // end namespace mlir
