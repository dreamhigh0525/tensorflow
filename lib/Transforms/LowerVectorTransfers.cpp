//===- LowerVectorTransfers.cpp - LowerVectorTransfers Pass Impl *- C++ -*-===//
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
// This file implements target-dependent lowering of vector transfer operations.
//
//===----------------------------------------------------------------------===//

#include <type_traits>

#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/Analysis/MLFunctionMatcher.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/Analysis/VectorAnalysis.h"
#include "mlir/EDSC/MLIREmitter.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/Pass.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/SuperVectorOps/SuperVectorOps.h"
#include "mlir/Support/Functional.h"
#include "mlir/Transforms/MLPatternLoweringPass.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

///
/// Implements lowering of VectorTransferReadOp and VectorTransferWriteOp to a
/// proper abstraction for the hardware.
///
/// For now only a simple loop nest is emitted.
///

using llvm::dbgs;
using llvm::SetVector;

using namespace mlir;

#define DEBUG_TYPE "lower-vector-transfers"

/// This function emits the proper Value* at the place of insertion of b,
/// where each value is the proper ConstantOp or DimOp. Returns a vector with
/// these Value*. Note this function does not concern itself with hoisting of
/// constants and will produce redundant IR. Subsequent MLIR simplification
/// passes like LICM and CSE are expected to clean this up.
///
/// More specifically, a MemRefType has a shape vector in which:
///   - constant ranks are embedded explicitly with their value;
///   - symbolic ranks are represented implicitly by -1 and need to be recovered
///     with a DimOp operation.
///
/// Example:
/// When called on:
///
/// ```mlir
///    memref<?x3x4x?x5xf32>
/// ```
///
/// This emits MLIR similar to:
///
/// ```mlir
///    %d0 = dim %0, 0 : memref<?x3x4x?x5xf32>
///    %c3 = constant 3 : index
///    %c4 = constant 4 : index
///    %d1 = dim %0, 0 : memref<?x3x4x?x5xf32>
///    %c5 = constant 5 : index
/// ```
///
/// and returns the vector with {%d0, %c3, %c4, %d1, %c5}.
bool isDynamicSize(int size) { return size < 0; }
SmallVector<Value *, 8> getMemRefSizes(FuncBuilder *b, Location loc,
                                       Value *memRef) {
  auto memRefType = memRef->getType().template cast<MemRefType>();
  SmallVector<Value *, 8> res;
  res.reserve(memRefType.getShape().size());
  unsigned countSymbolicShapes = 0;
  for (int size : memRefType.getShape()) {
    if (isDynamicSize(size)) {
      res.push_back(b->create<DimOp>(loc, memRef, countSymbolicShapes++));
    } else {
      res.push_back(b->create<ConstantIndexOp>(loc, size));
    }
  }
  return res;
}

namespace {
/// Helper structure to hold information about loop nest, clipped accesses to
/// the original scalar MemRef as well as full accesses to temporary MemRef in
/// local storage.
struct VectorTransferAccessInfo {
  // `ivs` are bound for `For` Stmt at `For` Stmt construction time.
  llvm::SmallVector<edsc::Bindable, 8> ivs;
  llvm::SmallVector<edsc::Expr, 8> lowerBoundsExprs;
  llvm::SmallVector<edsc::Expr, 8> upperBoundsExprs;
  llvm::SmallVector<edsc::Expr, 8> stepExprs;
  llvm::SmallVector<edsc::Expr, 8> clippedScalarAccessExprs;
  llvm::SmallVector<edsc::Expr, 8> tmpAccessExprs;
};

template <typename VectorTransferOpTy> class VectorTransferRewriter {
public:
  /// Perform the rewrite using the `emitter`.
  VectorTransferRewriter(VectorTransferOpTy *transfer,
                         MLFuncLoweringRewriter *rewriter,
                         MLFuncGlobalLoweringState *state);

  /// Perform the rewrite using the `emitter`.
  void rewrite();

  /// Helper class which creates clipped memref accesses to support lowering of
  /// the vector_transfer operation.
  VectorTransferAccessInfo makeVectorTransferAccessInfo();

private:
  VectorTransferOpTy *transfer;
  MLFuncLoweringRewriter *rewriter;
  MLFuncGlobalLoweringState *state;

  MemRefType memrefType;
  ArrayRef<int> memrefShape;
  VectorType vectorType;
  ArrayRef<int> vectorShape;
  AffineMap permutationMap;

  /// Used for staging the transfer in a local scalar buffer.
  MemRefType tmpMemRefType;
  /// View of tmpMemRefType as one vector, used in vector load/store to tmp
  /// buffer.
  MemRefType vectorMemRefType;

  // EDSC `emitter` and Bindables that are pre-bound at construction time.
  // vectorSizes are bound to the actual constant sizes of vectorType.
  llvm::SmallVector<edsc::Bindable, 8> vectorSizes;
  // accesses are bound to transfer->getIndices()
  llvm::SmallVector<edsc::Bindable, 8> accesses;
  // `zero` and `one` are bound to locally scoped constants.
  // `scalarMemRef` is bound to `transfer->getMemRef()`.
  edsc::Bindable zero, one, scalarMemRef;
  edsc::MLIREmitter emitter;
};

} // end anonymous namespace

/// Consider the case:
///
/// ```mlir {.mlir}
///    // Read the slice `%A[%i0, %i1:%i1+256, %i2:%i2+32]` into
///    // vector<32x256xf32> and pad with %f0 to handle the boundary case:
///    %f0 = constant 0.0f : f32
///    for %i0 = 0 to %0 {
///      for %i1 = 0 to %1 step 256 {
///        for %i2 = 0 to %2 step 32 {
///          %v = vector_transfer_read %A, %i0, %i1, %i2, %f0
///               {permutation_map: (d0, d1, d2) -> (d2, d1)} :
///               (memref<?x?x?xf32>, index, index, f32) -> vector<32x256xf32>
///    }}}
/// ```
///
/// The following constructs the `loadAccessExpr` that supports the emission of
/// MLIR resembling:
///
/// ```mlir
///    for %d1 = 0 to 256 {
///      for %d2 = 0 to 32 {
///        %s = %A[%i0, %i1 + %d1, %i2 + %d2] : f32
///        %tmp[%d2, %d1] = %s
///      }
///    }
/// ```
///
/// Notice in particular the order of loops iterating over the vector size
/// (i.e. 256x32 instead of 32x256). This results in contiguous accesses along
/// the most minor dimension of the original scalar tensor. On many hardware
/// architectures this will result in better utilization of the underlying
/// memory subsystem (e.g. prefetchers, DMAs, #memory transactions, etc...).
///
/// This additionally performs clipping as described in
/// `VectorTransferRewriter<VectorTransferReadOp>::rewrite` by emitting:
///
/// ```mlir-dsc
///    select(i + ii < zero, zero, select(i + ii < N, i + ii, N - one))
/// ```
template <typename VectorTransferOpTy>
VectorTransferAccessInfo
VectorTransferRewriter<VectorTransferOpTy>::makeVectorTransferAccessInfo() {
  using namespace mlir::edsc;

  // Create Bindable objects for ivs, they will be bound at `For` Stmt
  // construction.
  auto ivs = makeBindables(vectorShape.size());

  // Create and bind Bindables to refer to the Value for memref sizes.
  auto memRefSizes = makeBindables(memrefShape.size());
  auto memrefSizeValues = getMemRefSizes(
      emitter.getBuilder(), emitter.getLocation(), transfer->getMemRef());
  assert(memrefSizeValues.size() == memRefSizes.size());
  // Bind
  emitter.bindZipRange(llvm::zip(memRefSizes, memrefSizeValues));

  // Create the edsc::Expr for the clipped and transposes access expressions
  // using the permutationMap. Additionally, capture the index accessing the
  // most minor dimension.
  int coalescingIndex = -1;
  auto clippedScalarAccessExprs = makeExprs(accesses);
  auto tmpAccessExprs = makeExprs(ivs);
  for (auto it : llvm::enumerate(permutationMap.getResults())) {
    if (auto affineExpr = it.value().template dyn_cast<AffineDimExpr>()) {
      auto pos = affineExpr.getPosition();
      auto i = clippedScalarAccessExprs[pos];
      auto ii = ivs[it.index()];
      auto N = memRefSizes[pos];
      clippedScalarAccessExprs[pos] =
          select(i + ii < zero, zero, select(i + ii < N, i + ii, N - one));
      if (pos == clippedScalarAccessExprs.size() - 1) {
        // If a result of the permutation_map accesses the most minor dimension
        // then we record it.
        coalescingIndex = it.index();
      }
    } else {
      // Sanity check.
      assert(it.value().template cast<AffineConstantExpr>().getValue() == 0 &&
             "Expected dim or 0 in permutationMap");
    }
  }

  // Create the proper bindables for lbs, ubs and steps. Additionally, if we
  // recorded a coalescing index, permute the loop informations.
  auto lbs = makeBindables(ivs.size());
  auto ubs = makeExprs(vectorSizes);
  auto steps = makeBindables(ivs.size());
  if (coalescingIndex >= 0) {
    std::swap(ivs[coalescingIndex], ivs.back());
    std::swap(lbs[coalescingIndex], lbs.back());
    std::swap(ubs[coalescingIndex], ubs.back());
    std::swap(steps[coalescingIndex], steps.back());
  }
  emitter
      .template bindZipRangeConstants<ConstantIndexOp>(
          llvm::zip(lbs, SmallVector<int, 8>(ivs.size(), 0)))
      .template bindZipRangeConstants<ConstantIndexOp>(
          llvm::zip(steps, SmallVector<int, 8>(ivs.size(), 1)));

  return VectorTransferAccessInfo{ivs,
                                  makeExprs(lbs),
                                  ubs,
                                  makeExprs(steps),
                                  clippedScalarAccessExprs,
                                  tmpAccessExprs};
}

template <typename VectorTransferOpTy>
VectorTransferRewriter<VectorTransferOpTy>::VectorTransferRewriter(
    VectorTransferOpTy *transfer, MLFuncLoweringRewriter *rewriter,
    MLFuncGlobalLoweringState *state)
    : transfer(transfer), rewriter(rewriter), state(state),
      memrefType(transfer->getMemRefType()), memrefShape(memrefType.getShape()),
      vectorType(transfer->getVectorType()), vectorShape(vectorType.getShape()),
      permutationMap(transfer->getPermutationMap()),
      tmpMemRefType(
          MemRefType::get(vectorShape, vectorType.getElementType(), {}, 0)),
      vectorMemRefType(MemRefType::get({1}, vectorType, {}, 0)),
      vectorSizes(edsc::makeBindables(vectorShape.size())),
      emitter(edsc::MLIREmitter(rewriter->getBuilder(), transfer->getLoc())) {
  // Bind the Bindable.
  SmallVector<Value *, 8> transferIndices(transfer->getIndices());
  accesses = edsc::makeBindables(transferIndices.size());
  emitter.bind(scalarMemRef, transfer->getMemRef())
      .template bindConstant<ConstantIndexOp>(zero, 0)
      .template bindConstant<ConstantIndexOp>(one, 1)
      .template bindZipRangeConstants<ConstantIndexOp>(
          llvm::zip(vectorSizes, vectorShape))
      .template bindZipRange(llvm::zip(accesses, transfer->getIndices()));
};

/// Lowers VectorTransferReadOp into a combination of:
///   1. local memory allocation;
///   2. perfect loop nest over:
///      a. scalar load from local buffers (viewed as a scalar memref);
///      a. scalar store to original memref (with clipping).
///   3. vector_load from local buffer (viewed as a memref<1 x vector>);
///   4. local memory deallocation.
///
/// Lowers the data transfer part of a VectorTransferReadOp while ensuring no
/// out-of-bounds accesses are possible. Out-of-bounds behavior is handled by
/// clipping. This means that a given value in memory can be read multiple
/// times and concurrently.
///
/// Important notes about clipping and "full-tiles only" abstraction:
/// =================================================================
/// When using clipping for dealing with boundary conditions, the same edge
/// value will appear multiple times (a.k.a edge padding). This is fine if the
/// subsequent vector operations are all data-parallel but **is generally
/// incorrect** in the presence of reductions or extract operations.
///
/// More generally, clipping is a scalar abstraction that is expected to work
/// fine as a baseline for CPUs and GPUs but not for vector_load and DMAs.
/// To deal with real vector_load and DMAs, a "padded allocation + view"
/// abstraction with the ability to read out-of-memref-bounds (but still within
/// the allocated region) is necessary.
///
/// Whether using scalar loops or vector_load/DMAs to perform the transfer,
/// junk values will be materialized in the vectors and generally need to be
/// filtered out and replaced by the "neutral element". This neutral element is
/// op-dependent so, in the future, we expect to create a vector filter and
/// apply it to a splatted constant vector with the proper neutral element at
/// each ssa-use. This filtering is not necessary for pure data-parallel
/// operations.
///
/// In the case of vector_store/DMAs, Read-Modify-Write will be required, which
/// also have concurrency implications. Note that by using clipped scalar stores
/// in the presence of data-parallel only operations, we generate code that
/// writes the same value multiple time on the edge locations.
///
/// TODO(ntv): implement alternatives to clipping.
/// TODO(ntv): support non-data-parallel operations.
template <> void VectorTransferRewriter<VectorTransferReadOp>::rewrite() {
  using namespace mlir::edsc;

  // Build the AccessInfo which contain all the information needed to build the
  // perfectly nest loop nest to perform clipped reads and local writes.
  auto accessInfo = makeVectorTransferAccessInfo();

  // clang-format off
  auto &ivs = accessInfo.ivs;
  auto &lbs = accessInfo.lowerBoundsExprs;
  auto &ubs = accessInfo.upperBoundsExprs;
  auto &steps = accessInfo.stepExprs;
  Stmt scalarValue, vectorValue, tmpAlloc, tmpDealloc, vectorView;
  Stmt block = edsc::Block({
    tmpAlloc = alloc(tmpMemRefType),
    vectorView = vector_type_cast(tmpAlloc, vectorMemRefType),
    ForNest(ivs, lbs, ubs, steps, {
      scalarValue = load(scalarMemRef, accessInfo.clippedScalarAccessExprs),
      store(scalarValue, tmpAlloc, accessInfo.tmpAccessExprs),
    }),
    vectorValue = load(vectorView, zero),
    tmpDealloc = dealloc(tmpAlloc.getLHS())});
  // clang-format on

  // Emit the MLIR.
  emitter.emitStmt(block);

  // Finalize rewriting.
  transfer->replaceAllUsesWith(emitter.getValue(vectorValue.getLHS()));
  transfer->erase();
}

/// Lowers VectorTransferWriteOp into a combination of:
///   1. local memory allocation;
///   2. vector_store to local buffer (viewed as a memref<1 x vector>);
///   3. perfect loop nest over:
///      a. scalar load from local buffers (viewed as a scalar memref);
///      a. scalar store to original memref (with clipping).
///   4. local memory deallocation.
///
/// More specifically, lowers the data transfer part while ensuring no
/// out-of-bounds accesses are possible. Out-of-bounds behavior is handled by
/// clipping. This means that a given value in memory can be written to multiple
/// times and concurrently.
///
/// See `Important notes about clipping and full-tiles only abstraction` in the
/// description of `readClipped` above.
///
/// TODO(ntv): implement alternatives to clipping.
/// TODO(ntv): support non-data-parallel operations.
template <> void VectorTransferRewriter<VectorTransferWriteOp>::rewrite() {
  using namespace mlir::edsc;

  // Build the AccessInfo which contain all the information needed to build the
  // perfectly nest loop nest to perform local reads and clipped writes.
  auto accessInfo = makeVectorTransferAccessInfo();

  // Bind vector value for the vector_transfer_write.
  Bindable vectorValue;
  emitter.bind(vectorValue, transfer->getVector());

  // clang-format off
  auto &ivs = accessInfo.ivs;
  auto &lbs = accessInfo.lowerBoundsExprs;
  auto &ubs = accessInfo.upperBoundsExprs;
  auto &steps = accessInfo.stepExprs;
  Stmt scalarValue, tmpAlloc, tmpDealloc, vectorView;
  Stmt block = edsc::Block({
    tmpAlloc = alloc(tmpMemRefType),
    vectorView = vector_type_cast(tmpAlloc, vectorMemRefType),
    store(vectorValue, vectorView, {zero}),
    ForNest(ivs, lbs, ubs, steps, {
      scalarValue = load(tmpAlloc, accessInfo.tmpAccessExprs),
      store(scalarValue, scalarMemRef, accessInfo.clippedScalarAccessExprs),
    }),
    tmpDealloc = dealloc(tmpAlloc.getLHS())});
  // clang-format on

  // Emit the MLIR.
  emitter.emitStmt(block);

  // Finalize rewriting.
  transfer->erase();
}

namespace {
template <typename VectorTransferOpTy>
class VectorTransferExpander : public MLLoweringPattern {
public:
  explicit VectorTransferExpander(MLIRContext *context)
      : MLLoweringPattern(VectorTransferOpTy::getOperationName(), 1, context) {}

  PatternMatchResult match(OperationInst *op) const override {
    if (m_Op<VectorTransferOpTy>().match(op))
      return matchSuccess();
    return matchFailure();
  }
  void rewriteOpInst(OperationInst *op,
                     MLFuncGlobalLoweringState *funcWiseState,
                     std::unique_ptr<PatternState> opState,
                     MLFuncLoweringRewriter *rewriter) const override {
    VectorTransferRewriter<VectorTransferOpTy>(
        &*op->dyn_cast<VectorTransferOpTy>(), rewriter, funcWiseState)
        .rewrite();
  }
};

struct LowerVectorTransfersPass
    : public MLPatternLoweringPass<
          VectorTransferExpander<VectorTransferReadOp>,
          VectorTransferExpander<VectorTransferWriteOp>> {
  LowerVectorTransfersPass()
      : MLPatternLoweringPass(&LowerVectorTransfersPass::passID) {}

  // Thread-safe RAII context with local scope. BumpPtrAllocator freed on exit.
  edsc::ScopedEDSCContext raiiContext;

  static char passID;
};

} // end anonymous namespace

char LowerVectorTransfersPass::passID = 0;

FunctionPass *mlir::createLowerVectorTransfersPass() {
  return new LowerVectorTransfersPass();
}

static PassRegistration<LowerVectorTransfersPass>
    pass("lower-vector-transfers", "Materializes vector transfer ops to a "
                                   "proper abstraction for the hardware");

#undef DEBUG_TYPE
