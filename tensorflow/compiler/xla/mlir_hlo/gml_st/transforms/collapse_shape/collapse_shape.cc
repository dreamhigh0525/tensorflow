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

#include <memory>
#include <string>
#include <utility>

#include "gml_st/transforms/passes.h"
#include "gml_st/transforms/transforms.h"
#include "gml_st/utils/linalg_utils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

namespace mlir {
namespace gml_st {
namespace {

#define GEN_PASS_DEF_COLLAPSESHAPEPASS
#include "gml_st/transforms/passes.h.inc"

// Creates reassociation indices for `shape_collapse` and `shape_expand` ops.
// Given `rank`(N) and `retainTrailingDims`(M), returns the following
// reassociation:
//     [[0, 1, ..., N-M-1], [N-M], [N-M+1], ..., [N-1]]
//                         |--- retainTrailingDims ---|
//     |-------------------- rank --------------------|
SmallVector<ReassociationIndices> getCollapsingReassociationIndices(
    int64_t rank, int64_t retainTrailingDims) {
  SmallVector<ReassociationIndices> reassociation;
  reassociation.reserve(retainTrailingDims + 1);
  auto seq = llvm::seq<int64_t>(0, rank - retainTrailingDims);
  reassociation.emplace_back(seq.begin(), seq.end());
  for (int64_t i = rank - retainTrailingDims; i < rank; ++i)
    reassociation.push_back({i});
  return reassociation;
}

struct CollapseBcastPattern : OpRewritePattern<linalg::GenericOp> {
  using OpRewritePattern<linalg::GenericOp>::OpRewritePattern;

  CollapseBcastPattern(MLIRContext* ctx, int64_t retainTrailingDims)
      : OpRewritePattern<linalg::GenericOp>(ctx),
        retainTrailingDims(retainTrailingDims) {}

  LogicalResult matchAndRewrite(linalg::GenericOp op,
                                PatternRewriter& rewriter) const override {
    int64_t bcastDim;
    if (!isSimpleBcast(op, &bcastDim)) {
      return rewriter.notifyMatchFailure(op, "not a bcast op");
    }

    Value init = op.getOutputs().front();
    auto initTy = init.getType().cast<RankedTensorType>();
    int64_t initRank = initTy.getRank();

    if (initRank <= retainTrailingDims + 1) {
      return rewriter.notifyMatchFailure(op, "no dimension to collapse");
    }

    if (initRank - 1 - bcastDim >= retainTrailingDims) {
      return rewriter.notifyMatchFailure(op,
                                         "bcast dimension must be retained");
    }

    Value operand = op.getInputs().front();
    auto operandTy = operand.getType().cast<RankedTensorType>();
    int64_t operandRank = operandTy.getRank();

    // Collapse operand and init tensor.
    // For bcasts, this retains the last `retainTrailingDims` dimensions of the
    // *result* and collapses all others.
    Location loc = op.getLoc();
    SmallVector<ReassociationIndices> operandReassociation =
        getCollapsingReassociationIndices(operandRank, retainTrailingDims - 1);
    Value collapsedOperand = rewriter.createOrFold<tensor::CollapseShapeOp>(
        loc, operand, operandReassociation);
    SmallVector<ReassociationIndices> initReassosiation =
        getCollapsingReassociationIndices(initRank, retainTrailingDims);
    Value collapsedInit =
        rewriter.create<tensor::CollapseShapeOp>(loc, init, initReassosiation);

    auto collapsedInitTy = collapsedInit.getType().cast<RankedTensorType>();
    int64_t collapsedInitRank = collapsedInitTy.getRank();

    // Create collapsed bcast op.
    MLIRContext* ctx = getContext();
    AffineMap collapsedInitMap =
        AffineMap::getMultiDimIdentityMap(collapsedInitRank, ctx);
    int64_t collapsedBcastDim = bcastDim - initRank + collapsedInitRank;
    AffineMap collapsedOperandMap =
        collapsedInitMap.dropResult(collapsedBcastDim);
    SmallVector<AffineMap> collapsedMaps = {collapsedOperandMap,
                                            collapsedInitMap};
    SmallVector<utils::IteratorType> collapsedIteratorTypes(
        collapsedInitRank, utils::IteratorType::parallel);
    auto collapsedBcastOp = rewriter.create<linalg::GenericOp>(
        loc, collapsedInitTy, collapsedOperand, collapsedInit, collapsedMaps,
        collapsedIteratorTypes);
    collapsedBcastOp.getRegion().takeBody(op.getBodyRegion());

    // Re-expand reduction op and replace the original.
    auto reexpandedBcastOp = rewriter.create<tensor::ExpandShapeOp>(
        loc, initTy, collapsedBcastOp.getResult(0), initReassosiation);
    rewriter.replaceOp(op, reexpandedBcastOp.getResult());
    return success();
  }

 private:
  int64_t retainTrailingDims;
};

struct CollapseReductionPattern : OpRewritePattern<linalg::GenericOp> {
  using OpRewritePattern<linalg::GenericOp>::OpRewritePattern;

  CollapseReductionPattern(MLIRContext* ctx, int64_t retainTrailingDims)
      : OpRewritePattern<linalg::GenericOp>(ctx),
        retainTrailingDims(retainTrailingDims) {}

  LogicalResult matchAndRewrite(linalg::GenericOp op,
                                PatternRewriter& rewriter) const override {
    int64_t reductionDim;
    if (!isSimpleReduction(op, &reductionDim)) {
      return rewriter.notifyMatchFailure(op, "not a reduction");
    }

    Value operand = op.getInputs().front();
    auto operandTy = operand.getType().cast<RankedTensorType>();
    int64_t operandRank = operandTy.getRank();

    if (operandRank <= retainTrailingDims + 1) {
      return rewriter.notifyMatchFailure(op, "no dimension to collapse");
    }

    if (operandRank - 1 - reductionDim >= retainTrailingDims) {
      return rewriter.notifyMatchFailure(
          op, "reduction dimension must be retained");
    }

    Value init = op.getOutputs().front();
    auto initTy = init.getType().cast<RankedTensorType>();
    int64_t initRank = initTy.getRank();

    // Collapse operand and init tensor.
    // For reductions, this retains the last `retainTrailingDims` dimensions of
    // the *operand* and collapses all others.
    Location loc = op.getLoc();
    SmallVector<ReassociationIndices> operandReassociation =
        getCollapsingReassociationIndices(operandRank, retainTrailingDims);
    Value collapsedOperand = rewriter.create<tensor::CollapseShapeOp>(
        loc, operand, operandReassociation);
    SmallVector<ReassociationIndices> initReassosiation =
        getCollapsingReassociationIndices(initRank, retainTrailingDims - 1);
    Value collapsedInit =
        rewriter.create<tensor::CollapseShapeOp>(loc, init, initReassosiation);

    auto collapsedOperandTy =
        collapsedOperand.getType().cast<RankedTensorType>();
    int64_t collapsedOperandRank = collapsedOperandTy.getRank();
    auto collapsedInitTy = collapsedInit.getType().cast<RankedTensorType>();

    // Create collapsed reduction op.
    MLIRContext* ctx = op.getContext();
    AffineMap collapsedOperandMap =
        AffineMap::getMultiDimIdentityMap(collapsedOperandRank, ctx);
    int64_t collapsedReductionDim =
        reductionDim - operandRank + collapsedOperandRank;
    AffineMap collapsedInitMap =
        collapsedOperandMap.dropResult(collapsedReductionDim);
    SmallVector<AffineMap> collapsedMaps = {collapsedOperandMap,
                                            collapsedInitMap};
    SmallVector<utils::IteratorType> collapsedIteratorTypes(
        collapsedOperandRank, utils::IteratorType::parallel);
    collapsedIteratorTypes[collapsedReductionDim] =
        utils::IteratorType::reduction;
    auto collapsedReductionOp = rewriter.create<linalg::GenericOp>(
        loc, collapsedInitTy, collapsedOperand, collapsedInit, collapsedMaps,
        collapsedIteratorTypes);
    collapsedReductionOp.getRegion().takeBody(op.getBodyRegion());

    // Re-expand reduction op and replace the original.
    auto reexpandedReductionOp = rewriter.create<tensor::ExpandShapeOp>(
        loc, initTy, collapsedReductionOp.getResult(0), initReassosiation);
    rewriter.replaceOp(op, reexpandedReductionOp.getResult());
    return success();
  }

 private:
  int64_t retainTrailingDims;
};

struct CollapseCwisePattern : OpRewritePattern<linalg::GenericOp> {
  using OpRewritePattern<linalg::GenericOp>::OpRewritePattern;

  CollapseCwisePattern(MLIRContext* ctx, int64_t retainTrailingDims)
      : OpRewritePattern<linalg::GenericOp>(ctx),
        retainTrailingDims(retainTrailingDims) {}

  LogicalResult matchAndRewrite(linalg::GenericOp op,
                                PatternRewriter& rewriter) const override {
    if (!isCwiseGenericOp(op)) {
      return rewriter.notifyMatchFailure(op, "not a cwise op");
    }

    Value init = op.getOutputs().front();
    auto initTy = init.getType().cast<RankedTensorType>();
    int64_t rank = initTy.getRank();

    if (rank <= retainTrailingDims + 1) {
      return rewriter.notifyMatchFailure(op, "no dimension to collapse");
    }

    // Collapsed operands and init tensor.
    Location loc = op.getLoc();
    SmallVector<ReassociationIndices> reassociation =
        getCollapsingReassociationIndices(rank, retainTrailingDims);
    SmallVector<Value> collapsedOperands =
        llvm::to_vector(llvm::map_range(op.getInputs(), [&](Value it) -> Value {
          return rewriter.create<tensor::CollapseShapeOp>(loc, it,
                                                          reassociation);
        }));
    Value collapsedInit =
        rewriter.create<tensor::CollapseShapeOp>(loc, init, reassociation);

    auto collapsedInitTy = collapsedInit.getType().cast<RankedTensorType>();
    int64_t collapsedRank = collapsedInitTy.getRank();

    // Create collapsed cwise op.
    AffineMap collapsedIdentityMap =
        AffineMap::getMultiDimIdentityMap(collapsedRank, getContext());
    SmallVector<AffineMap> collapsedMaps(collapsedOperands.size() + 1,
                                         collapsedIdentityMap);
    SmallVector<utils::IteratorType> collapsedIteratorTypes(
        collapsedRank, utils::IteratorType::parallel);
    auto collapsedCwiseOp = rewriter.create<linalg::GenericOp>(
        loc, collapsedInitTy, collapsedOperands, collapsedInit, collapsedMaps,
        collapsedIteratorTypes);
    collapsedCwiseOp.getRegion().takeBody(op.getBodyRegion());

    // Re-expand cwise op and replace the original.
    Value reexpandedCwiseOp = rewriter.createOrFold<tensor::ExpandShapeOp>(
        loc, initTy, collapsedCwiseOp.getResult(0), reassociation);
    rewriter.replaceOp(op, reexpandedCwiseOp);
    return success();
  }

 private:
  int64_t retainTrailingDims;
};

struct CollapseShapePass
    : public impl::CollapseShapePassBase<CollapseShapePass> {
  using CollapseShapePassBase<CollapseShapePass>::CollapseShapePassBase;

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    MLIRContext* ctx = &getContext();

    // Populate shape-collapsing patterns for cwise ops, reductions, and bcasts.
    RewritePatternSet patterns(ctx);
    patterns.add<CollapseBcastPattern, CollapseCwisePattern,
                 CollapseReductionPattern>(ctx, retainTrailingDims);

    // Collect some related canonicalization patterns.
    tensor::CollapseShapeOp::getCanonicalizationPatterns(patterns, ctx);
    tensor::EmptyOp::getCanonicalizationPatterns(patterns, ctx);
    tensor::ExpandShapeOp::getCanonicalizationPatterns(patterns, ctx);
    linalg::FillOp::getCanonicalizationPatterns(patterns, ctx);
    linalg::GenericOp::getCanonicalizationPatterns(patterns, ctx);

    if (failed(applyPatternsAndFoldGreedily(f, std::move(patterns)))) {
      return signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>> createCollapseShapePass() {
  return std::make_unique<CollapseShapePass>();
}

std::unique_ptr<OperationPass<func::FuncOp>> createCollapseShapePass(
    const CollapseShapePassOptions& options) {
  return std::make_unique<CollapseShapePass>(options);
}

}  // namespace gml_st
}  // namespace mlir
