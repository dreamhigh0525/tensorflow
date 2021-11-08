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

#ifndef TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_DISC_RAL_TRANSFORMS_PASSES_H_
#define TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_DISC_RAL_TRANSFORMS_PASSES_H_

#include <memory>

#include "llvm/ADT/ArrayRef.h"

namespace mlir {

class FunctionPass;
class ModuleOp;
template <typename T>
class OperationPass;

namespace disc_ral {

std::unique_ptr<OperationPass<ModuleOp>> createRalInjectExecutionContextPass(
    const std::string& entry_func_name = "main");

// Lower some specific ops to library calls (modeled by `disc_ral.launch` op).
std::unique_ptr<mlir::FunctionPass> createRalLowerToLibraryCallPass();

// Lower disc to llvm dialect
std::unique_ptr<OperationPass<ModuleOp>> createRalToLLVMPass();

}  // namespace disc_ral

}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_DISC_RAL_TRANSFORMS_PASSES_H_
