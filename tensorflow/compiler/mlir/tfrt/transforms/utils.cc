/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/tfrt/transforms/utils.h"

#include <optional>
#include <string>

#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/OperationSupport.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tfrt/basic_kernels/opdefs/tfrt_base.h"  // from @tf_runtime
#include "tfrt/basic_kernels/opdefs/types.h"  // from @tf_runtime
#include "tfrt/core_runtime/opdefs/types.h"  // from @tf_runtime

namespace tensorflow {

bool IsResourceArgument(mlir::Value value) {
  auto arg = value.dyn_cast<mlir::BlockArgument>();
  if (!arg) return false;

  auto func = llvm::cast<mlir::func::FuncOp>(arg.getOwner()->getParentOp());

  return func.getArgAttr(arg.getArgNumber(), "tf.resource_name") != nullptr;
}

bool IsResultVariable(const mlir::Value &original_operand,
                      const mlir::Value &operand) {
  if (original_operand.isa<mlir::OpResult>()) {
    auto defining_op = original_operand.getDefiningOp();

    // TODO(b/174753886): When device assignment is properly done, we
    // should check that TF::ReadVariableOp is for TPU device here.
    if (llvm::isa<mlir::TF::ReadVariableOp>(defining_op) &&
        defining_op->getNumOperands() == 1) {
      return true;
    } else if (llvm::isa<mlir::TF::_TfrtGetResourceOp>(defining_op)) {
      return true;
    }
    return false;
  }
  return IsResourceArgument(operand);
}

std::optional<std::string> CanonicalizeTensorflowFunctionName(
    const mlir::SymbolTable &symbol_table, absl::string_view mlir_func_name,
    bool use_mlir_func_name) {
  if (use_mlir_func_name) {
    return std::string(mlir_func_name);
  }

  // Currently in TF graph to MLIR importing, a "0" is appended to the original
  // function name. The renaming is for TF/XLA v1 bridge use cases. Refer to
  // b/142268695, b/141617294 for more context.
  //
  // TFRT currently uses the original function library. Hence, we retrieve the
  // original function name from the function attributes. Longer term, we
  // probably want to export the MLIR functions.
  auto callee =
      symbol_table.lookup<mlir::func::FuncOp>(std::string(mlir_func_name));
  if (!callee) return std::nullopt;

  mlir::StringAttr original_func_name =
      callee->getAttrOfType<mlir::StringAttr>("tf._original_func_name");
  if (!original_func_name) {
    // If there is no function attribute "tf._original_func_name" in the callee,
    // we use the workaround to recover the original function name by removing
    // the last char of the MLIR function name.
    // TODO(b/259138201): Remove this workwaround after we make sure
    // "tf._original_func_name" is present in callees in all code paths.
    mlir_func_name.remove_suffix(1);
    return std::string(mlir_func_name);
  }

  return original_func_name.str();
}

}  // namespace tensorflow
