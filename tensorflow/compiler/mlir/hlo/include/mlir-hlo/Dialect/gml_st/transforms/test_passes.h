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

#ifndef TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_GML_ST_TRANSFORMS_TEST_PASSES_H_
#define TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_GML_ST_TRANSFORMS_TEST_PASSES_H_

#include "mlir/Pass/Pass.h"

namespace mlir {
namespace gml_st {

std::unique_ptr<OperationPass<FuncOp>> createTestGmlStLoopPeelingPass();

std::unique_ptr<OperationPass<FuncOp>> createTestGmlStLoopTilingPass();

#define GEN_PASS_REGISTRATION
#include "mlir-hlo/Dialect/gml_st/transforms/test_passes.h.inc"

}  // namespace gml_st
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_DIALECT_GML_ST_TRANSFORMS_TEST_PASSES_H_
