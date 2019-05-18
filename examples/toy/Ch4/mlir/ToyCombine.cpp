//===- ToyCombine.cpp - Toy High Level Optimizer --------------------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements a simple combiner for optimizing pattern in the Toy
// dialect.
//
//===----------------------------------------------------------------------===//

#include "toy/Dialect.h"

#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"

#include <numeric>

namespace toy {

namespace {

/// Fold transpose(transpose(x) -> transpose(x)
struct SimplifyRedundantTranspose : public mlir::RewritePattern {
  /// We register this pattern to match every toy.transpose in the IR.
  /// The "benefit" is used by the framework to order the patterns and process
  /// them in order of profitability.
  SimplifyRedundantTranspose(mlir::MLIRContext *context)
      : RewritePattern(TransposeOp::getOperationName(), /* benefit = */ 1,
                       context) {}

  /// This method is attempting to match a pattern and rewrite it. The rewriter
  /// argument is the orchestrator of the sequence of rewrites. It is expected
  /// to interact with it to perform any changes to the IR from here.
  mlir::PatternMatchResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    // We can directly cast the current operation as this will only get invoked
    // on TransposeOp.
    TransposeOp transpose = llvm::cast<TransposeOp>(op);
    // Look through the input of the current transpose.
    mlir::Value *transposeInput = transpose.getOperand();
    TransposeOp transposeInputOp =
        llvm::dyn_cast_or_null<TransposeOp>(transposeInput->getDefiningOp());
    // If the input is defined by another Transpose, bingo!
    if (!transposeInputOp)
      return matchFailure();

    // Use the rewriter to perform the replacement
    rewriter.replaceOp(op, {transposeInputOp.getOperand()}, {transposeInputOp});
    return matchSuccess();
  }
};

/// Fold reshape(constant(x)) -> constant(x'), with x' being reshaped in place.
struct SimplifyReshapeConstant : public mlir::RewritePattern {
  SimplifyReshapeConstant(mlir::MLIRContext *context)
      : RewritePattern(ReshapeOp::getOperationName(), /* benefit = */ 1,
                       context) {}

  mlir::PatternMatchResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    ReshapeOp reshape = llvm::cast<ReshapeOp>(op);
    // Look through the input of the current reshape.
    ConstantOp constantOp = llvm::dyn_cast_or_null<ConstantOp>(
        reshape.getOperand()->getDefiningOp());
    // If the input is defined by another constant, bingo!
    if (!constantOp)
      return matchFailure();

    auto reshapeType = op->getResult(0)->getType().cast<ToyArrayType>();
    if (auto valueAttr =
            constantOp.getAttrOfType<mlir::DenseElementsAttr>("value")) {
      // FIXME Check matching of element count!
      //      auto oldType = constantOp.getType();
      auto newType = rewriter.getTensorType(
          reshapeType.getShape(), valueAttr.getType().getElementType());
      auto newAttr =
          mlir::DenseElementsAttr::get(newType, valueAttr.getRawData());
      rewriter.replaceOpWithNewOp<ConstantOp>(op, reshapeType.getShape(),
                                              newAttr);
    } else if (auto valueAttr =
                   constantOp.getAttrOfType<mlir::FloatAttr>("value")) {
      // Broadcast
      auto dataSize = std::accumulate(reshapeType.getShape().begin(),
                                      reshapeType.getShape().end(), 1,
                                      std::multiplies<int>());
      std::vector<mlir::Attribute> data(dataSize, valueAttr);
      auto tensorTy = rewriter.getTensorType(reshapeType.getShape(),
                                             reshapeType.getElementType());
      auto newAttr = mlir::DenseElementsAttr::get(tensorTy, data);
      rewriter.replaceOpWithNewOp<ConstantOp>(op, reshapeType.getShape(),
                                              newAttr);
    } else {
      llvm_unreachable("Unsupported Constant format");
    }
    return matchSuccess();
  }
};

/// Fold reshape(reshape(x)) -> reshape(x)
struct SimplifyReshapeReshape : public mlir::RewritePattern {
  SimplifyReshapeReshape(mlir::MLIRContext *context)
      : RewritePattern(ReshapeOp::getOperationName(), /* benefit = */ 1,
                       context) {}

  mlir::PatternMatchResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    ReshapeOp reshape = llvm::cast<ReshapeOp>(op);
    // Look through the input of the current reshape.
    mlir::Value *reshapeInput = reshape.getOperand();
    // If the input is defined by another reshape, bingo!
    if (!matchPattern(reshapeInput, mlir::m_Op<ReshapeOp>()))
      return matchFailure();

    // Use the rewriter to perform the replacement
    rewriter.replaceOp(op, {reshapeInput});
    return matchSuccess();
  }
};

/// Fold reshape(x)) -> x, when input type matches output type
struct SimplifyNullReshape : public mlir::RewritePattern {
  SimplifyNullReshape(mlir::MLIRContext *context)
      : RewritePattern(ReshapeOp::getOperationName(), /* benefit = */ 1,
                       context) {}

  mlir::PatternMatchResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    ReshapeOp reshape = llvm::cast<ReshapeOp>(op);
    if (reshape.getOperand()->getType() != reshape.getResult()->getType())
      return matchFailure();
    rewriter.replaceOp(reshape, {reshape.getOperand()});
    return matchSuccess();
  }
};

} // end anonymous namespace.

// Register our patterns for rewrite by the Canonicalization framework.
void TransposeOp::getCanonicalizationPatterns(
    mlir::OwningRewritePatternList &results, mlir::MLIRContext *context) {
  results.push_back(llvm::make_unique<SimplifyRedundantTranspose>(context));
}

// Register our patterns for rewrite by the Canonicalization framework.
void ReshapeOp::getCanonicalizationPatterns(
    mlir::OwningRewritePatternList &results, mlir::MLIRContext *context) {
  mlir::RewriteListBuilder<SimplifyReshapeConstant, SimplifyReshapeReshape,
                           SimplifyNullReshape>::build(results, context);
}

} // namespace toy
