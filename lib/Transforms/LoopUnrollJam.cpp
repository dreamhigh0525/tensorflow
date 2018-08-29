//===- LoopUnrollAndJam.cpp - Code to perform loop unroll jam
//----------------===//
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
// This file implements loop unroll jam for MLFunctions. Unroll and jam is a
// transformation that improves locality, in particular, register reuse, while
// also improving instruction level parallelism. The example below shows what it
// does in nearly the general case. Loop unroll jam currently works if the
// bounds of the loops inner to the loop being unroll-jammed do not depend on
// the latter.
//
// Before      After unroll-jam of i by factor 2:
//
//             for i, step = 2
// for i         S1(i);
//   S1;         S2(i);
//   S2;         S1(i+1);
//   for j       S2(i+1);
//     S3;       for j
//     S4;         S3(i, j);
//   S5;           S4(i, j);
//   S6;           S3(i+1, j)
//                 S4(i+1, j)
//               S5(i);
//               S6(i);
//               S5(i+1);
//               S6(i+1);
//
// Note: 'if/else' blocks are not jammed. So, if there are loops inside if
// stmt's, bodies of those loops will not be jammed.
//
//===----------------------------------------------------------------------===//
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/StandardOps.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/Transforms/Pass.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CommandLine.h"

using namespace mlir;
using namespace llvm::cl;

// Loop unroll jam factor.
static llvm::cl::opt<unsigned>
    clUnrollJamFactor("unroll-jam-factor", llvm::cl::Hidden,
                      llvm::cl::desc("Use this unroll jam factor for all loops"
                                     " (default 4)"));

namespace {
/// Loop unroll jam pass. For test purposes, this just unroll jams the first
/// outer loop in an MLFunction.
struct LoopUnrollAndJam : public MLFunctionPass {
  Optional<unsigned> unrollJamFactor;
  static const unsigned kDefaultUnrollJamFactor = 4;

  explicit LoopUnrollAndJam(Optional<unsigned> unrollJamFactor)
      : unrollJamFactor(unrollJamFactor) {}

  void runOnMLFunction(MLFunction *f) override;
  bool runOnForStmt(ForStmt *forStmt);
  bool loopUnrollJamByFactor(ForStmt *forStmt, unsigned unrollJamFactor);
};
} // end anonymous namespace

MLFunctionPass *mlir::createLoopUnrollAndJamPass(int unrollJamFactor) {
  return new LoopUnrollAndJam(
      unrollJamFactor == -1 ? None : Optional<unsigned>(unrollJamFactor));
}

void LoopUnrollAndJam::runOnMLFunction(MLFunction *f) {
  // Currently, just the outermost loop from the first loop nest is
  // unroll-and-jammed by this pass. However, runOnForStmt can be called on any
  // for Stmt.
  if (!isa<ForStmt>(f->begin()))
    return;

  auto *forStmt = cast<ForStmt>(f->begin());
  runOnForStmt(forStmt);
}

/// Unroll and jam a 'for' stmt. Default unroll jam factor is
/// kDefaultUnrollJamFactor. Return false if nothing was done.
bool LoopUnrollAndJam::runOnForStmt(ForStmt *forStmt) {
  // Unroll and jam by the factor that was passed if any.
  if (unrollJamFactor.hasValue())
    return loopUnrollJamByFactor(forStmt, unrollJamFactor.getValue());
  // Otherwise, unroll jam by the command-line factor if one was specified.
  if (clUnrollJamFactor.getNumOccurrences() > 0)
    return loopUnrollJamByFactor(forStmt, clUnrollJamFactor);

  // Unroll and jam by four otherwise.
  return loopUnrollJamByFactor(forStmt, kDefaultUnrollJamFactor);
}

/// Unrolls and jams this loop by the specified factor.
bool LoopUnrollAndJam::loopUnrollJamByFactor(ForStmt *forStmt,
                                             unsigned unrollJamFactor) {
  assert(unrollJamFactor >= 1 && "unroll jam factor should be >= 1");

  if (unrollJamFactor == 1 || forStmt->getStatements().empty())
    return false;

  if (!forStmt->hasConstantBounds())
    return false;

  // Gathers all maximal sub-blocks of statements that do not themselves include
  // a for stmt (a statement could have a descendant for stmt though in its
  // tree).
  class JamBlockGatherer : public StmtWalker<JamBlockGatherer> {
  public:
    typedef llvm::iplist<Statement> StmtListType;

    // Store iterators to the first and last stmt of each sub-block found.
    std::vector<std::pair<StmtBlock::iterator, StmtBlock::iterator>> subBlocks;

    // This is a linear time walk.
    void walk(StmtListType::iterator Start, StmtListType::iterator End) {
      for (auto it = Start; it != End;) {
        auto subBlockStart = it;
        while (it != End && !isa<ForStmt>(it))
          ++it;
        if (it != subBlockStart)
          // Record the last statement (one behind the iterator) while not
          // changing the iterator position.
          subBlocks.push_back({subBlockStart, (--it)++});
        // Process all for Stmts that appear next.
        while (it != End && isa<ForStmt>(it))
          walkForStmt(cast<ForStmt>(it++));
      }
    }
  };

  auto lb = forStmt->getConstantLowerBound();
  auto ub = forStmt->getConstantUpperBound();
  auto step = forStmt->getStep();

  int64_t tripCount = (ub - lb + 1) % step == 0 ? (ub - lb + 1) / step
                                                : (ub - lb + 1) / step + 1;

  // If the trip count is lower than the unroll jam factor, no unrolled body.
  // TODO(bondhugula): option to specify cleanup loop unrolling.
  if (tripCount < unrollJamFactor)
    return true;

  // Gather all sub-blocks to jam upon the loop being unrolled.
  JamBlockGatherer jbg;
  jbg.walkForStmt(forStmt);
  auto &subBlocks = jbg.subBlocks;

  // Generate the cleanup loop if trip count isn't a multiple of
  // unrollJamFactor.
  if (tripCount % unrollJamFactor) {
    DenseMap<const MLValue *, MLValue *> operandMap;
    // Insert the cleanup loop right after 'forStmt'.
    MLFuncBuilder builder(forStmt->getBlock(), ++StmtBlock::iterator(forStmt));
    auto *cleanupForStmt = cast<ForStmt>(builder.clone(*forStmt, operandMap));
    cleanupForStmt->setConstantLowerBound(
        lb + (tripCount - tripCount % unrollJamFactor) * step);
  }

  MLFuncBuilder b(forStmt);
  forStmt->setStep(step * unrollJamFactor);
  forStmt->setConstantUpperBound(
      lb + (tripCount - tripCount % unrollJamFactor - 1) * step);

  for (auto &subBlock : subBlocks) {
    // Builder to insert unroll-jammed bodies. Insert right at the end of
    // sub-block.
    MLFuncBuilder builder(subBlock.first->getBlock(),
                          std::next(subBlock.second));

    // Unroll and jam (appends unrollJamFactor-1 additional copies).
    for (unsigned i = 1; i < unrollJamFactor; i++) {
      DenseMap<const MLValue *, MLValue *> operandMapping;

      // If the induction variable is used, create a remapping to the value for
      // this unrolled instance.
      if (!forStmt->use_empty()) {
        // iv' = iv + i, i = 1 to unrollJamFactor-1.
        auto *bumpExpr = builder.getAddExpr(builder.getDimExpr(0),
                                            builder.getConstantExpr(i * step));
        auto *bumpMap = builder.getAffineMap(1, 0, {bumpExpr}, {});
        auto *ivUnroll =
            builder.create<AffineApplyOp>(forStmt->getLoc(), bumpMap, forStmt)
                ->getResult(0);
        operandMapping[forStmt] = cast<MLValue>(ivUnroll);
      }
      // Clone the sub-block being unroll-jammed (this doesn't include the last
      // stmt because subBlock.second is inclusive).
      for (auto it = subBlock.first; it != subBlock.second; ++it) {
        builder.clone(*it, operandMapping);
      }
      // Clone the last statement of the sub-block.
      builder.clone(*subBlock.second, operandMapping);
    }
  }
  return true;
}
