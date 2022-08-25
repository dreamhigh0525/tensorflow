/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

// This file implements logic for lowering LHLO GPU dialect to TFRT CUDA
// dialect.

#include "tensorflow/compiler/mlir/tfrt/transforms/lmhlo_to_gpu/lmhlo_to_gpu_binary.h"

#include <memory>
#include <utility>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emitter_unnested.h"

namespace tensorflow {

void populateKernelOpsPattern(mlir::RewritePatternSet&, GpuBinaryOptions);

namespace {

#define GEN_PASS_CLASSES
#include "tensorflow/compiler/mlir/tfrt/transforms/lmhlo_to_gpu/gpu_passes.h.inc"

struct ConvertLmhloToGpuBinaryPass
    : public ConvertLmhloToGpuBinaryPassBase<ConvertLmhloToGpuBinaryPass> {
 public:
  explicit ConvertLmhloToGpuBinaryPass(GpuBinaryOptions options)
      : options(options) {}

 private:
  void runOnOperation() override {
    mlir::RewritePatternSet patterns(&getContext());
    populateKernelOpsPattern(patterns, options);
    if (failed(applyOpPatternsAndFold(getOperation(), std::move(patterns))))
      return signalPassFailure();
  }

  void getDependentDialects(mlir::DialectRegistry& registry) const override {
    xla::gpu::IrEmitterUnnested::GetDependentDialects(registry);
  }

  GpuBinaryOptions options;
};

}  // namespace

std::unique_ptr<mlir::Pass> createConvertLmhloToGpuBinaryPass(
    GpuBinaryOptions options) {
  return std::make_unique<ConvertLmhloToGpuBinaryPass>(options);
}

void registerConvertLmhloToGpuBinaryPass() {
  ::mlir::registerPass([] { return createConvertLmhloToGpuBinaryPass(); });
}

}  // namespace tensorflow
