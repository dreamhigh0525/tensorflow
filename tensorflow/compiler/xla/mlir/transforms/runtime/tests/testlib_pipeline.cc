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

#include "tensorflow/compiler/xla/mlir/transforms/runtime/tests/testlib_pipeline.h"

#include <utility>

#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"  // from @llvm-project
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"  // from @llvm-project
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"  // from @llvm-project
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/SCF/IR/SCF.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir/transforms/runtime/passes.h"

namespace xla {
namespace runtime {

void RegisterXlaRuntimeTestlibDialects(mlir::DialectRegistry& registry) {
  // Register MLIR dialects supported by the Xla runtime tests.
  registry.insert<mlir::arith::ArithmeticDialect, mlir::scf::SCFDialect,
                  mlir::func::FuncDialect, RuntimeDialect>();

  // Register MLIR dialects that can be translated to LLVM IR.
  registerLLVMDialectTranslation(registry);
}

void CreateXlaRuntimeTestlibPipeline(mlir::OpPassManager& pm) {
  pm.addPass(mlir::createConvertSCFToCFPass());

  // Convert entry function to the XLA entrypoint.
  pm.addPass(CreateConvertToEntrypoint());

  // Convert runtime operations and custom calls to LLVM dialect.
  ConvertRuntimeToLLvmOpts rt_to_llvm_opts;
  pm.addPass(CreateConvertRuntimeToLLVMPass(std::move(rt_to_llvm_opts)));

  // Convert everything else to LLVM dialect.
  pm.addPass(mlir::createConvertFuncToLLVMPass());
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());
}

}  // namespace runtime
}  // namespace xla
