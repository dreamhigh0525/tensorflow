//===- MemRefBoundCheck.cpp - MLIR Affine Structures Class-----*- C++ -*-===//
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
// This file implements a pass to check memref accessses for out of bound
// accesses.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/Passes.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/Pass.h"
#include "mlir/StandardOps/StandardOps.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "memref-bound-check"

using namespace mlir;

namespace {

/// Checks for out of bound memef access subscripts..
struct MemRefBoundCheck : public FunctionPass, StmtWalker<MemRefBoundCheck> {
  explicit MemRefBoundCheck() : FunctionPass(&MemRefBoundCheck::passID) {}

  PassResult runOnMLFunction(MLFunction *f) override;
  // Not applicable to CFG functions.
  PassResult runOnCFGFunction(CFGFunction *f) override { return success(); }

  void visitOperationStmt(OperationStmt *opStmt);

  static char passID;
};

} // end anonymous namespace

char MemRefBoundCheck::passID = 0;

FunctionPass *mlir::createMemRefBoundCheckPass() {
  return new MemRefBoundCheck();
}


void MemRefBoundCheck::visitOperationStmt(OperationStmt *opStmt) {
  // TODO(bondhugula): extend this to store's and other memref dereferencing
  // op's.
  if (auto loadOp = opStmt->dyn_cast<LoadOp>()) {
    MemRefRegion region;
    if (!getMemRefRegion(opStmt, /*loopDepth=*/0, &region))
      return;
    LLVM_DEBUG(llvm::dbgs() << "Memory region");
    LLVM_DEBUG(region.getConstraints()->dump());
    unsigned rank = loadOp->getMemRefType().getRank();
    // For each dimension, check for out of bounds.
    for (unsigned r = 0; r < rank; r++) {
      FlatAffineConstraints ucst(*region.getConstraints());
      // Intersect memory region with constraint capturing out of bounds,
      // and check if the constraint system is feasible. If it is, there is at
      // least one point out of bounds.
      SmallVector<int64_t, 4> ineq(rank + 1, 0);
      int dimSize = loadOp->getMemRefType().getDimSize(r);
      // TODO(bondhugula): handle dynamic dim sizes.
      if (dimSize == -1)
        continue;
      // d_i >= memref dim size.
      ucst.addConstantLowerBound(r, dimSize);
      LLVM_DEBUG(llvm::dbgs() << "System to check for overflow:\n");
      LLVM_DEBUG(ucst.dump());
      //
      if (!ucst.isEmpty()) {
        loadOp->emitOpError(
            "memref out of upper bound access along dimension #" +
            Twine(r + 1));
      }
      // Check for less than negative index.
      FlatAffineConstraints lcst(*region.getConstraints());
      std::fill(ineq.begin(), ineq.end(), 0);
      // d_i <= -1;
      lcst.addConstantUpperBound(r, -1);
      LLVM_DEBUG(llvm::dbgs() << "System to check for underflow:\n");
      LLVM_DEBUG(lcst.dump());
      if (!lcst.isEmpty()) {
        loadOp->emitOpError(
            "memref out of lower bound access along dimension #" +
            Twine(r + 1));
      }
    }
  }
}

PassResult MemRefBoundCheck::runOnMLFunction(MLFunction *f) {
  return walk(f), success();
}

static PassRegistration<MemRefBoundCheck>
    memRefBoundCheck("memref-bound-check",
                     "Check memref accesses in an MLFunction");
