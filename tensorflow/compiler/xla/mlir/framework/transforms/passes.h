/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_MLIR_FRAMEWORK_TRANSFORMS_PASSES_H_
#define TENSORFLOW_COMPILER_XLA_MLIR_FRAMEWORK_TRANSFORMS_PASSES_H_

#include <memory>

#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project

namespace mlir {

namespace func {
class FuncOp;
}  // namespace func
template <typename T>
class OperationPass;

namespace xla_framework {

// Wrap function with XLA:CPU's C interface.
std::unique_ptr<OperationPass<ModuleOp>> CreateOutlineWithXLAFrameworkPass();

// Convert XLAFramework operations to LLVM operations.
std::unique_ptr<OperationPass<ModuleOp>> CreateLegalizeXLAFrameworkToLLVMPass();

#define GEN_PASS_REGISTRATION
#define GEN_PASS_DECL_LEGALIZEXLAFRAMEWORKTOLLVM
#define GEN_PASS_DECL_OUTLINEWITHXLAFRAMEWORK
#include "tensorflow/compiler/xla/mlir/framework/transforms/passes.h.inc"

}  // namespace xla_framework
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_XLA_MLIR_FRAMEWORK_TRANSFORMS_PASSES_H_
