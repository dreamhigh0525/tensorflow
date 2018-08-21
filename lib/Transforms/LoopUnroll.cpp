//===- Unroll.cpp - Code to perform loop unrolling ------------------------===//
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
// This file implements loop unrolling.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/CFGFunction.h"
#include "mlir/IR/MLFunction.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OperationSet.h"
#include "mlir/IR/StandardOps.h"
#include "mlir/IR/Statements.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/Transforms/Pass.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace llvm;

// Loop unrolling factor.
static llvm::cl::opt<unsigned>
    clUnrollFactor("unroll-factor", cl::Hidden,
                   cl::desc("Use this unroll factor for all loops"));

static llvm::cl::opt<bool> clUnrollFull("unroll-full", cl::Hidden,
                                        cl::desc("Fully unroll loops"));

static llvm::cl::opt<unsigned> clUnrollFullThreshold(
    "unroll-full-threshold", cl::Hidden,
    cl::desc("Unroll all loops with trip count less than or equal to this"));

namespace {
/// Loop unrolling pass. Unrolls all innermost loops unless full unrolling and a
/// full unroll threshold was specified, in which case, fully unrolls all loops
/// with trip count less than the specified threshold. The latter is for testing
/// purposes, especially for testing outer loop unrolling.
struct LoopUnroll : public MLFunctionPass {
  Optional<unsigned> unrollFactor;
  Optional<bool> unrollFull;

  explicit LoopUnroll(Optional<unsigned> unrollFactor,
                      Optional<bool> unrollFull)
      : unrollFactor(unrollFactor), unrollFull(unrollFull) {}

  void runOnMLFunction(MLFunction *f) override;
  /// Unroll this for stmt. Returns false if nothing was done.
  bool runOnForStmt(ForStmt *forStmt);
  bool loopUnrollFull(ForStmt *forStmt);
  bool loopUnrollByFactor(ForStmt *forStmt, unsigned unrollFactor);
};
} // end anonymous namespace

MLFunctionPass *mlir::createLoopUnrollPass(int unrollFactor, int unrollFull) {
  return new LoopUnroll(unrollFactor == -1 ? None
                                           : Optional<unsigned>(unrollFactor),
                        unrollFull == -1 ? None : Optional<bool>(unrollFull));
}

void LoopUnroll::runOnMLFunction(MLFunction *f) {
  // Gathers all innermost loops through a post order pruned walk.
  class InnermostLoopGatherer : public StmtWalker<InnermostLoopGatherer, bool> {
  public:
    // Store innermost loops as we walk.
    std::vector<ForStmt *> loops;

    // This method specialized to encode custom return logic.
    typedef llvm::iplist<Statement> StmtListType;
    bool walkPostOrder(StmtListType::iterator Start,
                       StmtListType::iterator End) {
      bool hasInnerLoops = false;
      // We need to walk all elements since all innermost loops need to be
      // gathered as opposed to determining whether this list has any inner
      // loops or not.
      while (Start != End)
        hasInnerLoops |= walkPostOrder(&(*Start++));
      return hasInnerLoops;
    }

    bool walkForStmtPostOrder(ForStmt *forStmt) {
      bool hasInnerLoops = walkPostOrder(forStmt->begin(), forStmt->end());
      if (!hasInnerLoops)
        loops.push_back(forStmt);
      return true;
    }

    bool walkIfStmtPostOrder(IfStmt *ifStmt) {
      bool hasInnerLoops =
          walkPostOrder(ifStmt->getThen()->begin(), ifStmt->getThen()->end());
      hasInnerLoops |=
          walkPostOrder(ifStmt->getElse()->begin(), ifStmt->getElse()->end());
      return hasInnerLoops;
    }

    bool visitOperationStmt(OperationStmt *opStmt) { return false; }

    // FIXME: can't use base class method for this because that in turn would
    // need to use the derived class method above. CRTP doesn't allow it, and
    // the compiler error resulting from it is also misleading.
    using StmtWalker<InnermostLoopGatherer, bool>::walkPostOrder;
  };

  // Gathers all loops with trip count <= minTripCount.
  class ShortLoopGatherer : public StmtWalker<ShortLoopGatherer> {
  public:
    // Store short loops as we walk.
    std::vector<ForStmt *> loops;
    const unsigned minTripCount;
    ShortLoopGatherer(unsigned minTripCount) : minTripCount(minTripCount) {}

    void visitForStmt(ForStmt *forStmt) {
      auto lb = forStmt->getLowerBound()->getValue();
      auto ub = forStmt->getUpperBound()->getValue();
      auto step = forStmt->getStep();

      if ((ub - lb) / step + 1 <= minTripCount)
        loops.push_back(forStmt);
    }
  };

  if (clUnrollFull.getNumOccurrences() > 0 &&
      clUnrollFullThreshold.getNumOccurrences() > 0) {
    ShortLoopGatherer slg(clUnrollFullThreshold);
    // Do a post order walk so that loops are gathered from innermost to
    // outermost (or else unrolling an outer one may delete gathered inner
    // ones).
    slg.walkPostOrder(f);
    auto &loops = slg.loops;
    for (auto *forStmt : loops)
      loopUnrollFull(forStmt);
    return;
  }

  InnermostLoopGatherer ilg;
  ilg.walkPostOrder(f);
  auto &loops = ilg.loops;
  for (auto *forStmt : loops)
    runOnForStmt(forStmt);
}

/// Unroll a for stmt. Default unroll factor is 4.
bool LoopUnroll::runOnForStmt(ForStmt *forStmt) {
  // Unroll completely if full loop unroll was specified.
  if (clUnrollFull.getNumOccurrences() > 0 ||
      (unrollFull.hasValue() && unrollFull.getValue()))
    return loopUnrollFull(forStmt);

  // Unroll by the specified factor if one was specified.
  if (clUnrollFactor.getNumOccurrences() > 0)
    return loopUnrollByFactor(forStmt, clUnrollFactor);
  else if (unrollFactor.hasValue())
    return loopUnrollByFactor(forStmt, unrollFactor.getValue());

  // Unroll by four otherwise.
  return loopUnrollByFactor(forStmt, 4);
}

// Unrolls this loop completely.
bool LoopUnroll::loopUnrollFull(ForStmt *forStmt) {
  auto lb = forStmt->getLowerBound()->getValue();
  auto ub = forStmt->getUpperBound()->getValue();
  auto step = forStmt->getStep();

  // Builder to add constants need for the unrolled iterator.
  auto *mlFunc = forStmt->findFunction();
  MLFuncBuilder funcTopBuilder(&mlFunc->front());

  // Builder to insert the unrolled bodies.  We insert right after the
  /// ForStmt we're unrolling.
  MLFuncBuilder builder(forStmt->getBlock(), ++StmtBlock::iterator(forStmt));

  // Unroll the contents of 'forStmt'.
  for (int64_t i = lb; i <= ub; i += step) {
    DenseMap<const MLValue *, MLValue *> operandMapping;

    // If the induction variable is used, create a constant for this unrolled
    // value and add an operand mapping for it.
    if (!forStmt->use_empty()) {
      auto *ivConst =
          funcTopBuilder.create<ConstantAffineIntOp>(i)->getResult();
      operandMapping[forStmt] = cast<MLValue>(ivConst);
    }

    // Clone the body of the loop.
    for (auto &childStmt : *forStmt) {
      builder.clone(childStmt, operandMapping);
    }
  }
  // Erase the original 'for' stmt from the block.
  forStmt->eraseFromBlock();
  return true;
}

/// Unrolls this loop by the specified unroll factor.
bool LoopUnroll::loopUnrollByFactor(ForStmt *forStmt, unsigned unrollFactor) {
  assert(unrollFactor >= 1 && "unroll factor shoud be >= 1");

  if (unrollFactor == 1 || forStmt->getStatements().empty())
    return false;

  auto lb = forStmt->getLowerBound()->getValue();
  auto ub = forStmt->getUpperBound()->getValue();
  auto step = forStmt->getStep();

  int64_t tripCount = (int64_t)ceilf((ub - lb + 1) / (float)step);

  // If the trip count is lower than the unroll factor, no unrolled body.
  // TODO(bondhugula): option to specify cleanup loop unrolling.
  if (tripCount < unrollFactor)
    return true;

  // Generate the cleanup loop if trip count isn't a multiple of unrollFactor.
  if (tripCount % unrollFactor) {
    DenseMap<const MLValue *, MLValue *> operandMap;
    MLFuncBuilder builder(forStmt->getBlock(), ++StmtBlock::iterator(forStmt));
    auto *cleanupForStmt = cast<ForStmt>(builder.clone(*forStmt, operandMap));
    cleanupForStmt->setLowerBound(builder.getConstantExpr(
        lb + (tripCount - tripCount % unrollFactor) * step));
  }

  // Builder to insert unrolled bodies right after the last statement in the
  // body of 'forStmt'.
  MLFuncBuilder builder(forStmt, StmtBlock::iterator(forStmt->end()));
  forStmt->setStep(step * unrollFactor);
  forStmt->setUpperBound(builder.getConstantExpr(
      lb + (tripCount - tripCount % unrollFactor - 1) * step));

  // Keep a pointer to the last statement in the original block so that we know
  // what to clone (since we are doing this in-place).
  StmtBlock::iterator srcBlockEnd = --forStmt->end();

  // Unroll the contents of 'forStmt' (unrollFactor-1 additional copies
  // appended).
  for (unsigned i = 1; i < unrollFactor; i++) {
    DenseMap<const MLValue *, MLValue *> operandMapping;

    // If the induction variable is used, create a remapping to the value for
    // this unrolled instance.
    if (!forStmt->use_empty()) {
      // iv' = iv + 1/2/3...unrollFactor-1;
      auto *bumpExpr = builder.getAddExpr(builder.getDimExpr(0),
                                          builder.getConstantExpr(i * step));
      auto *bumpMap = builder.getAffineMap(1, 0, {bumpExpr}, {});
      auto *ivUnroll =
          builder.create<AffineApplyOp>(bumpMap, forStmt)->getResult(0);
      operandMapping[forStmt] = cast<MLValue>(ivUnroll);
    }

    // Clone the original body of the loop (this doesn't include the last stmt).
    for (auto it = forStmt->begin(); it != srcBlockEnd; it++) {
      builder.clone(*it, operandMapping);
    }
    // Clone the last statement in the original body.
    builder.clone(*srcBlockEnd, operandMapping);
  }
  return true;
}
