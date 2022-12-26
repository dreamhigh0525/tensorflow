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

#ifndef MLIR_HLO_DIALECT_GML_ST_TRANSFORMS_FUSION_H
#define MLIR_HLO_DIALECT_GML_ST_TRANSFORMS_FUSION_H

#include "gml_st/IR/gml_st_ops.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/IR/PatternMatch.h"

namespace mlir {
namespace gml_st {

// Create fused operation based on the specificed subset. The result is
// equivalent to the given `materialize` op.
FailureOr<Value> createFusedOp(PatternRewriter &rewriter,
                               MaterializeOp materializeOp);

// Fuses an op into `gml_st.materialize` and performs the necessary updates to
// the surrounding loop if any.
FailureOr<Operation *> fuse(PatternRewriter &rewriter,
                            MaterializeOp materializeOp);

// Finds `gml_st.materialize` ops in the block and fuses ops into them. Verifies
// that fusion candidate doesn't have any uses except the one
// `gml_st.materialize` in the block to avoid exponential code growth.
void fuseGreedily(PatternRewriter &rewriter, Block &block,
                  llvm::function_ref<bool(Operation *)> filterFn = nullptr);

/// Populate fusion patterns.
void populateFusionPatterns(MLIRContext *ctx,
                            function_ref<LogicalResult(MaterializeOp)> filterFn,
                            RewritePatternSet *patterns);

struct FusionCluster {
  DenseSet<Operation *> operations;
  Operation *root;
};

// Find a cluster of operations that can be tiled and fused together around
// the root op. We want to fuse output of the fusion op with elementwise ops. In
// general case a cluster is a tree that can have multiple leaf-node ops,
// e.g. map(op, map(op)).
// First element of the cluster is always the root for tiling.
template <class FusionOpTy>
FusionCluster findMapFusionCluster(FusionOpTy op) {
  // Find the root operation in the chain of elementwise ops. Current approach
  // doesn't work well if maps don't form a chain.
  Operation *rootOp = op;
  while (true) {
    auto users = llvm::to_vector(rootOp->getUsers());

    if (users.size() != 1) break;
    if (!isa<linalg::MapOp>(users[0])) break;

    rootOp = users[0];
  }

  // Run a graph search to find all linalg.map and that can be fused in
  // the root op.
  DenseSet<Operation *> resultOps;
  SmallVector<Operation *> remainingProducers{rootOp};

  while (!remainingProducers.empty()) {
    Operation *curOp = remainingProducers.pop_back_val();
    if (!curOp) continue;

    if (auto fusionOp = dyn_cast<FusionOpTy>(curOp)) {
      for (auto *u : fusionOp->getUsers())
        // Do not fuse fusionOp that is used by another fusionOp.
        if (isa<FusionOpTy>(u)) continue;
      resultOps.insert(curOp);
    } else if (auto mapOp = dyn_cast<linalg::MapOp>(curOp)) {
      resultOps.insert(curOp);
      for (auto *operand : mapOp.getDpsInputOperands())
        remainingProducers.push_back(operand->get().getDefiningOp());
    }
  }
  return {resultOps, rootOp};
}

template <class FusionOpTy>
LogicalResult fuseOutputFill(PatternRewriter &rewriter, FusionOpTy op) {
  // Fusion into the output.
  Operation *definingOp = op.getDpsInitOperand(0)->get().getDefiningOp();

  // linalg.fill has already been fused for another matmul.
  if (isa<linalg::FillOp>(definingOp)) return success();

  auto materialize = dyn_cast<MaterializeOp>(definingOp);
  if (!materialize) {
    return rewriter.notifyMatchFailure(
        op, "has failed to 'materialize' output during 'linalg.fill' fusion.");
  }
  if (materialize.getSource().getDefiningOp<linalg::FillOp>()) {
    if (failed(fuse(rewriter, materialize))) return failure();
  }
  return success();
}

}  // namespace gml_st
}  // namespace mlir

#endif  // MLIR_HLO_DIALECT_GML_ST_TRANSFORMS_FUSION_H
