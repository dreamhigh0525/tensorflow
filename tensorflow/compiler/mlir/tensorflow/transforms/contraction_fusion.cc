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

#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"

namespace mlir {
namespace TF {
namespace {

// -------------------------------------------------------------------------- //
// Fuse ContractionFusableInterface operations into MatMul operation.
// -------------------------------------------------------------------------- //

// TODO(ezhulenev): Parametrize this pattern by `BaseOp` and `FusedOp` to fuse
// different kinds of contractions (MatMul, Conv2D, etc...).

class FuseIntoMatMulOp : public RewritePattern {
 public:
  FuseIntoMatMulOp() : RewritePattern(PatternBenefit(1), MatchAnyOpTypeTag()) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    auto fusable = dyn_cast<ContractionFusableInterface>(op);
    if (!fusable) return failure();

    auto failed = [&](Twine message) -> LogicalResult {
      return rewriter.notifyMatchFailure(op, message);
    };

    // Check if the operation can be fused.
    Optional<ContractionFusion> fusion = fusable.GetContractionFusion();
    if (!fusion.hasValue()) {
      return failed("returned empty contraction fusion specification");
    }

    // Check if preceeding operation is a MatMul that we can use for fusion.
    // TODO(ezhulenev): Support fusing into _JitFusedMatMul.
    MatMulOp matmul = op->getOperand(0).getDefiningOp<MatMulOp>();
    if (!matmul) {
      return failed("input to the fusable op must be a MatMul");
    }
    if (!matmul.getResult().hasOneUse()) {
      return failed("MatMul result must have one use");
    }

    MLIRContext *ctx = op->getContext();

    // Build a fused MatMul operation from a base MatMul and a fusion.
    SmallVector<Location, 3> locations = {matmul.getLoc(), op->getLoc()};
    Location loc = rewriter.getFusedLoc(locations);

    // Fusion can't change the type of a base operation.
    Type result_ty = matmul.getType();

    // Copy all operands from a matmul and add additional fusion arguments.
    SmallVector<Value, 3> operands(matmul.getOperands());
    for (int idx : fusion->additional_arguments) {
      operands.push_back(op->getOperand(idx));
    }

    // Copy attributes from a MatMul operation.
    SmallVector<NamedAttribute, 4> attrs(matmul.getAttrs().begin(),
                                         matmul.getAttrs().end());

    // Add a fused output kernel name to the list of fusions.
    NamedAttribute fusion_attr(
        Identifier::get("fusion", ctx),
        ArrayAttr::get({StringAttr::get(fusion->output_kernel, ctx)}, ctx));
    attrs.push_back(fusion_attr);

    // Update all uses of a fusable op with a new fused operation.
    using FusedOp = _JitFusedMatMulOp;
    Value fused = rewriter.create<FusedOp>(loc, result_ty, operands, attrs);
    rewriter.replaceOp(op, {fused});

    return failure();
  }
};

// -------------------------------------------------------------------------- //

struct ContractionFusionPass
    : public PassWrapper<ContractionFusionPass, FunctionPass> {
  void runOnFunction() override;
};

void ContractionFusionPass::runOnFunction() {
  FuncOp func = getFunction();

  OwningRewritePatternList patterns;
  patterns.insert<FuseIntoMatMulOp>();
  applyPatternsAndFoldGreedily(func, patterns);
}

}  // namespace

std::unique_ptr<OperationPass<FuncOp>> CreateContractionFusionPass() {
  return std::make_unique<ContractionFusionPass>();
}

static PassRegistration<ContractionFusionPass> pass(
    "tf-contraction-fusion",
    "Fuses operations implementing ContractionFusionInterface into the "
    "contraction operations");

}  // namespace TF
}  // namespace mlir
