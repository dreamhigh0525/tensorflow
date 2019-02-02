//===- DmaGeneration.cpp - DMA generation pass ------------------------ -*-===//
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
// This file implements a pass to automatically promote accessed memref regions
// to buffers in a faster memory space that is explicitly managed, with the
// necessary data movement operations expressed as DMAs.
//
//===----------------------------------------------------------------------===//

#include "mlir/AffineOps/AffineOps.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <algorithm>

#define DEBUG_TYPE "dma-generate"

using namespace mlir;
using llvm::SmallMapVector;

static llvm::cl::OptionCategory clOptionsCategory(DEBUG_TYPE " options");

static llvm::cl::opt<unsigned> clFastMemorySpace(
    "dma-fast-mem-space", llvm::cl::Hidden,
    llvm::cl::desc("Set fast memory space id for DMA generation"),
    llvm::cl::cat(clOptionsCategory));

static llvm::cl::opt<uint64_t> clFastMemoryCapacity(
    "dma-fast-mem-capacity", llvm::cl::Hidden,
    llvm::cl::desc("Set fast memory space capacity in KiB"),
    llvm::cl::cat(clOptionsCategory));

namespace {

/// Generates DMAs for memref's living in 'slowMemorySpace' into newly created
/// buffers in 'fastMemorySpace', and replaces memory operations to the former
/// by the latter. Only load op's handled for now.
/// TODO(bondhugula): extend this to store op's.
struct DmaGeneration : public FunctionPass {
  explicit DmaGeneration(unsigned slowMemorySpace = 0,
                         unsigned fastMemorySpaceArg = 1,
                         int minDmaTransferSize = 1024)
      : FunctionPass(&DmaGeneration::passID), slowMemorySpace(slowMemorySpace),
        minDmaTransferSize(minDmaTransferSize) {
    if (clFastMemorySpace.getNumOccurrences() > 0) {
      fastMemorySpace = clFastMemorySpace;
    } else {
      fastMemorySpace = fastMemorySpaceArg;
    }
  }

  PassResult runOnFunction(Function *f) override;
  void runOnAffineForOp(OpPointer<AffineForOp> forOp);

  bool generateDma(const MemRefRegion &region, OpPointer<AffineForOp> forOp,
                   uint64_t *sizeInBytes);

  // List of memory regions to DMA for. We need a map vector to have a
  // guaranteed iteration order to write test cases. CHECK-DAG doesn't help here
  // since the alloc's for example are identical except for the SSA id.
  SmallMapVector<Value *, std::unique_ptr<MemRefRegion>, 4> readRegions;
  SmallMapVector<Value *, std::unique_ptr<MemRefRegion>, 4> writeRegions;

  // Map from original memref's to the DMA buffers that their accesses are
  // replaced with.
  DenseMap<Value *, Value *> fastBufferMap;

  // Slow memory space associated with DMAs.
  const unsigned slowMemorySpace;
  // Fast memory space associated with DMAs.
  unsigned fastMemorySpace;
  // Minimum DMA transfer size supported by the target in bytes.
  const int minDmaTransferSize;

  // Constant zero index to avoid too many duplicates.
  Value *zeroIndex = nullptr;

  static char passID;
};

} // end anonymous namespace

char DmaGeneration::passID = 0;

/// Generates DMAs for memref's living in 'slowMemorySpace' into newly created
/// buffers in 'fastMemorySpace', and replaces memory operations to the former
/// by the latter. Only load op's handled for now.
/// TODO(bondhugula): extend this to store op's.
FunctionPass *mlir::createDmaGenerationPass(unsigned slowMemorySpace,
                                            unsigned fastMemorySpace,
                                            int minDmaTransferSize) {
  return new DmaGeneration(slowMemorySpace, fastMemorySpace,
                           minDmaTransferSize);
}

// Info comprising stride and number of elements transferred every stride.
struct StrideInfo {
  int64_t stride;
  int64_t numEltPerStride;
};

/// Returns striding information for a copy/transfer of this region with
/// potentially multiple striding levels from outermost to innermost. For an
/// n-dimensional region, there can be at most n-1 levels of striding
/// successively nested.
//  TODO(bondhugula): make this work with non-identity layout maps.
static void getMultiLevelStrides(const MemRefRegion &region,
                                 ArrayRef<int64_t> bufferShape,
                                 SmallVectorImpl<StrideInfo> *strideInfos) {
  if (bufferShape.size() <= 1)
    return;

  int64_t numEltPerStride = 1;
  int64_t stride = 1;
  for (int d = bufferShape.size() - 1; d >= 1; d--) {
    int64_t dimSize = region.memref->getType().cast<MemRefType>().getDimSize(d);
    stride *= dimSize;
    numEltPerStride *= bufferShape[d];
    // A stride is needed only if the region has a shorter extent than the
    // memref along the dimension *and* has an extent greater than one along the
    // next major dimension.
    if (bufferShape[d] < dimSize && bufferShape[d - 1] > 1) {
      strideInfos->push_back({stride, numEltPerStride});
    }
  }
}

/// Construct the memref region to just include the entire memref. Returns false
/// dynamic shaped memref's for now. `numParamLoopIVs` is the number of
/// enclosing loop IVs of opInst (starting from the outermost) that the region
/// is parametric on.
static bool getFullMemRefAsRegion(OperationInst *opInst,
                                  unsigned numParamLoopIVs,
                                  MemRefRegion *region) {
  unsigned rank;
  if (auto loadOp = opInst->dyn_cast<LoadOp>()) {
    rank = loadOp->getMemRefType().getRank();
    region->memref = loadOp->getMemRef();
    region->setWrite(false);
  } else if (auto storeOp = opInst->dyn_cast<StoreOp>()) {
    rank = storeOp->getMemRefType().getRank();
    region->memref = storeOp->getMemRef();
    region->setWrite(true);
  } else {
    assert(false && "expected load or store op");
    return false;
  }
  auto memRefType = region->memref->getType().cast<MemRefType>();
  if (memRefType.getNumDynamicDims() > 0)
    return false;

  auto *regionCst = region->getConstraints();

  // Just get the first numSymbols IVs, which the memref region is parametric
  // on.
  SmallVector<OpPointer<AffineForOp>, 4> ivs;
  getLoopIVs(*opInst, &ivs);
  ivs.resize(numParamLoopIVs);
  SmallVector<Value *, 4> symbols = extractForInductionVars(ivs);
  regionCst->reset(rank, numParamLoopIVs, 0);
  regionCst->setIdValues(rank, rank + numParamLoopIVs, symbols);

  // Memref dim sizes provide the bounds.
  for (unsigned d = 0; d < rank; d++) {
    auto dimSize = memRefType.getDimSize(d);
    assert(dimSize > 0 && "filtered dynamic shapes above");
    regionCst->addConstantLowerBound(d, 0);
    regionCst->addConstantUpperBound(d, dimSize - 1);
  }
  return true;
}

// Creates a buffer in the faster memory space for the specified region;
// generates a DMA from the lower memory space to this one, and replaces all
// loads to load from that buffer. Returns false if DMAs could not be generated
// due to yet unimplemented cases.
bool DmaGeneration::generateDma(const MemRefRegion &region,
                                OpPointer<AffineForOp> forOp,
                                uint64_t *sizeInBytes) {
  auto *forInst = forOp->getInstruction();

  // DMAs for read regions are going to be inserted just before the for loop.
  FuncBuilder prologue(forInst);
  // DMAs for write regions are going to be inserted just after the for loop.
  FuncBuilder epilogue(forInst->getBlock(),
                       std::next(Block::iterator(forInst)));
  FuncBuilder *b = region.isWrite() ? &epilogue : &prologue;

  // Builder to create constants at the top level.
  FuncBuilder top(forInst->getFunction());

  auto loc = forInst->getLoc();
  auto *memref = region.memref;
  auto memRefType = memref->getType().cast<MemRefType>();

  auto layoutMaps = memRefType.getAffineMaps();
  if (layoutMaps.size() > 1 ||
      (layoutMaps.size() == 1 && !layoutMaps[0].isIdentity())) {
    LLVM_DEBUG(llvm::dbgs() << "Non-identity layout map not yet supported\n");
    return false;
  }

  // Indices to use for the DmaStart op.
  // Indices for the original memref being DMAed from/to.
  SmallVector<Value *, 4> memIndices;
  // Indices for the faster buffer being DMAed into/from.
  SmallVector<Value *, 4> bufIndices;

  unsigned rank = memRefType.getRank();
  SmallVector<int64_t, 4> fastBufferShape;

  // Compute the extents of the buffer.
  std::vector<SmallVector<int64_t, 4>> lbs;
  SmallVector<int64_t, 8> lbDivisors;
  lbs.reserve(rank);
  Optional<int64_t> numElements = region.getConstantBoundingSizeAndShape(
      &fastBufferShape, &lbs, &lbDivisors);
  if (!numElements.hasValue()) {
    LLVM_DEBUG(llvm::dbgs() << "Non-constant region size not supported\n");
    return false;
  }

  if (numElements.getValue() == 0) {
    LLVM_DEBUG(llvm::dbgs() << "Nothing to DMA\n");
    *sizeInBytes = 0;
    return true;
  }

  const FlatAffineConstraints *cst = region.getConstraints();
  // 'outerIVs' holds the values that this memory region is symbolic/paramteric
  // on; this would correspond to loop IVs surrounding the level at which the
  // DMA generation is being done.
  SmallVector<Value *, 8> outerIVs;
  cst->getIdValues(rank, cst->getNumIds(), &outerIVs);

  // Construct the index expressions for the fast memory buffer. The index
  // expression for a particular dimension of the fast buffer is obtained by
  // subtracting out the lower bound on the original memref's data region
  // along the corresponding dimension.

  // Index start offsets for faster memory buffer relative to the original.
  SmallVector<AffineExpr, 4> offsets;
  offsets.reserve(rank);
  for (unsigned d = 0; d < rank; d++) {
    assert(lbs[d].size() == cst->getNumCols() - rank && "incorrect bound size");

    AffineExpr offset = top.getAffineConstantExpr(0);
    for (unsigned j = 0, e = cst->getNumCols() - rank - 1; j < e; j++) {
      offset = offset + lbs[d][j] * top.getAffineDimExpr(j);
    }
    assert(lbDivisors[d] > 0);
    offset =
        (offset + lbs[d][cst->getNumCols() - 1 - rank]).floorDiv(lbDivisors[d]);

    // Set DMA start location for this dimension in the lower memory space
    // memref.
    if (auto caf = offset.dyn_cast<AffineConstantExpr>()) {
      auto indexVal = caf.getValue();
      if (indexVal == 0) {
        memIndices.push_back(zeroIndex);
      } else {
        memIndices.push_back(
            top.create<ConstantIndexOp>(loc, indexVal)->getResult());
      }
    } else {
      // The coordinate for the start location is just the lower bound along the
      // corresponding dimension on the memory region (stored in 'offset').
      auto map = top.getAffineMap(
          cst->getNumDimIds() + cst->getNumSymbolIds() - rank, 0, offset, {});
      memIndices.push_back(b->create<AffineApplyOp>(loc, map, outerIVs));
    }
    // The fast buffer is DMAed into at location zero; addressing is relative.
    bufIndices.push_back(zeroIndex);

    // Record the offsets since they are needed to remap the memory accesses of
    // the original memref further below.
    offsets.push_back(offset);
  }

  // The faster memory space buffer.
  Value *fastMemRef;

  // Check if a buffer was already created.
  // TODO(bondhugula): union across all memory op's per buffer. For now assuming
  // that multiple memory op's on the same memref have the *same* memory
  // footprint.
  if (fastBufferMap.count(memref) == 0) {
    auto fastMemRefType = top.getMemRefType(
        fastBufferShape, memRefType.getElementType(), {}, fastMemorySpace);

    LLVM_DEBUG(llvm::dbgs() << "Creating a new buffer of type: ");
    LLVM_DEBUG(fastMemRefType.dump(); llvm::dbgs() << "\n");

    // Create the fast memory space buffer just before the 'for' instruction.
    fastMemRef = prologue.create<AllocOp>(loc, fastMemRefType)->getResult();
    // Record it.
    fastBufferMap[memref] = fastMemRef;
    // fastMemRefType is a constant shaped memref.
    *sizeInBytes = getMemRefSizeInBytes(fastMemRefType).getValue();
    LLVM_DEBUG(llvm::dbgs() << "Creating a new buffer of type ";
               fastMemRefType.dump();
               llvm::dbgs()
               << " and size " << Twine(llvm::divideCeil(*sizeInBytes, 1024))
               << " KiB\n";);

  } else {
    // Reuse the one already created.
    fastMemRef = fastBufferMap[memref];
    *sizeInBytes = 0;
  }
  // Create a tag (single element 1-d memref) for the DMA.
  auto tagMemRefType = top.getMemRefType({1}, top.getIntegerType(32));
  auto tagMemRef = prologue.create<AllocOp>(loc, tagMemRefType);
  auto numElementsSSA =
      top.create<ConstantIndexOp>(loc, numElements.getValue());

  // TODO(bondhugula): check for transfer sizes not being a multiple of
  // minDmaTransferSize and handle them appropriately.

  SmallVector<StrideInfo, 4> strideInfos;
  getMultiLevelStrides(region, fastBufferShape, &strideInfos);

  // TODO(bondhugula): use all stride level once DmaStartOp is extended for
  // multi-level strides.
  if (strideInfos.size() > 1) {
    LLVM_DEBUG(llvm::dbgs() << "Only up to one level of stride supported\n");
    return false;
  }

  Value *stride = nullptr;
  Value *numEltPerStride = nullptr;
  if (!strideInfos.empty()) {
    stride = top.create<ConstantIndexOp>(loc, strideInfos[0].stride);
    numEltPerStride =
        top.create<ConstantIndexOp>(loc, strideInfos[0].numEltPerStride);
  }

  if (!region.isWrite()) {
    // DMA non-blocking read from original buffer to fast buffer.
    b->create<DmaStartOp>(loc, memref, memIndices, fastMemRef, bufIndices,
                          numElementsSSA, tagMemRef, zeroIndex, stride,
                          numEltPerStride);
  } else {
    // DMA non-blocking write from fast buffer to the original memref.
    b->create<DmaStartOp>(loc, fastMemRef, bufIndices, memref, memIndices,
                          numElementsSSA, tagMemRef, zeroIndex, stride,
                          numEltPerStride);
  }

  // Matching DMA wait to block on completion; tag always has a 0 index.
  b->create<DmaWaitOp>(loc, tagMemRef, zeroIndex, numElementsSSA);

  // Replace all uses of the old memref with the faster one while remapping
  // access indices (subtracting out lower bound offsets for each dimension).
  // Ex: to replace load %A[%i, %j] with load %Abuf[%i - %iT, %j - %jT],
  // index remap will be (%i, %j) -> (%i - %iT, %j - %jT),
  // i.e., affine_apply (d0, d1, d2, d3) -> (d2-d0, d3-d1) (%iT, %jT, %i, %j),
  // and (%iT, %jT) will be the 'extraOperands' for 'rep all memref uses with'.
  // d2, d3 correspond to the original indices (%i, %j).
  SmallVector<AffineExpr, 4> remapExprs;
  remapExprs.reserve(rank);
  for (unsigned i = 0; i < rank; i++) {
    // The starting operands of indexRemap will be outerIVs (the loops
    // surrounding the depth at which this DMA is being done); then those
    // corresponding to the memref's original indices follow.
    auto dimExpr = b->getAffineDimExpr(outerIVs.size() + i);
    remapExprs.push_back(dimExpr - offsets[i]);
  }
  auto indexRemap = b->getAffineMap(outerIVs.size() + rank, 0, remapExprs, {});
  // *Only* those uses within the body of 'forOp' are replaced.
  replaceAllMemRefUsesWith(memref, fastMemRef,
                           /*extraIndices=*/{}, indexRemap,
                           /*extraOperands=*/outerIVs,
                           /*domInstFilter=*/&*forOp->getBody()->begin());
  return true;
}

// TODO(bondhugula): make this run on a Block instead of a 'for' inst.
void DmaGeneration::runOnAffineForOp(OpPointer<AffineForOp> forOp) {
  // For now (for testing purposes), we'll run this on the outermost among 'for'
  // inst's with unit stride, i.e., right at the top of the tile if tiling has
  // been done. In the future, the DMA generation has to be done at a level
  // where the generated data fits in a higher level of the memory hierarchy; so
  // the pass has to be instantiated with additional information that we aren't
  // provided with at the moment.
  if (forOp->getStep() != 1) {
    auto *forBody = forOp->getBody();
    if (forBody->empty())
      return;
    if (auto innerFor =
            cast<OperationInst>(forBody->front()).dyn_cast<AffineForOp>()) {
      runOnAffineForOp(innerFor);
    }
    return;
  }

  // DMAs will be generated for this depth, i.e., for all data accessed by this
  // loop.
  unsigned dmaDepth = getNestingDepth(*forOp->getInstruction());

  readRegions.clear();
  writeRegions.clear();
  fastBufferMap.clear();

  // Walk this 'for' instruction to gather all memory regions.
  forOp->walkOps([&](OperationInst *opInst) {
    // Gather regions to promote to buffers in faster memory space.
    // TODO(bondhugula): handle store op's; only load's handled for now.
    if (auto loadOp = opInst->dyn_cast<LoadOp>()) {
      if (loadOp->getMemRefType().getMemorySpace() != slowMemorySpace)
        return;
    } else if (auto storeOp = opInst->dyn_cast<StoreOp>()) {
      if (storeOp->getMemRefType().getMemorySpace() != slowMemorySpace)
        return;
    } else {
      // Neither load nor a store op.
      return;
    }

    // TODO(bondhugula): eventually, we need to be performing a union across
    // all regions for a given memref instead of creating one region per
    // memory op. This way we would be allocating O(num of memref's) sets
    // instead of O(num of load/store op's).
    auto region = std::make_unique<MemRefRegion>();
    if (!getMemRefRegion(opInst, dmaDepth, region.get())) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Error obtaining memory region: semi-affine maps?\n");
      LLVM_DEBUG(llvm::dbgs() << "over-approximating to the entire memref\n");
      if (!getFullMemRefAsRegion(opInst, dmaDepth, region.get())) {
        LLVM_DEBUG(
            forOp->emitError("Non-constant memref sizes not yet supported"));
        return;
      }
    }

    // Each memref has a single buffer associated with it irrespective of how
    // many load's and store's happen on it.
    // TODO(bondhugula): in the future, when regions don't intersect and satisfy
    // other properties (based on load/store regions), we could consider
    // multiple buffers per memref.

    // Add to the appropriate region if it's not already in it, or take a
    // bounding box union with the existing one if it's already in there.
    // Note that a memref may have both read and write regions - so update the
    // region in the other list if one exists (write in case of read and vice
    // versa) since there is a single bounding box for a memref across all reads
    // and writes that happen on it.

    // Attempts to update; returns true if 'region' exists in targetRegions.
    auto updateRegion =
        [&](const SmallMapVector<Value *, std::unique_ptr<MemRefRegion>, 4>
                &targetRegions) {
          auto it = targetRegions.find(region->memref);
          if (it == targetRegions.end())
            return false;

          // Perform a union with the existing region.
          if (!(*it).second->unionBoundingBox(*region)) {
            LLVM_DEBUG(llvm::dbgs()
                       << "Memory region bounding box failed"
                          "over-approximating to the entire memref\n");
            if (!getFullMemRefAsRegion(opInst, dmaDepth, region.get())) {
              LLVM_DEBUG(forOp->emitError(
                  "Non-constant memref sizes not yet supported"));
            }
          }
          return true;
        };

    bool existsInRead = updateRegion(readRegions);
    bool existsInWrite = updateRegion(writeRegions);

    // Finally add it to the region list.
    if (region->isWrite() && !existsInWrite) {
      writeRegions[region->memref] = std::move(region);
    } else if (!region->isWrite() && !existsInRead) {
      readRegions[region->memref] = std::move(region);
    }
  });

  uint64_t totalSizeInBytes = 0;

  bool ret = true;
  auto processRegions =
      [&](const SmallMapVector<Value *, std::unique_ptr<MemRefRegion>, 4>
              &regions) {
        for (const auto &regionEntry : regions) {
          uint64_t sizeInBytes;
          bool iRet = generateDma(*regionEntry.second, forOp, &sizeInBytes);
          if (iRet)
            totalSizeInBytes += sizeInBytes;
          ret = ret & iRet;
        }
      };
  processRegions(readRegions);
  processRegions(writeRegions);
  if (!ret) {
    forOp->emitError("DMA generation failed for one or more memref's\n");
    return;
  }
  LLVM_DEBUG(llvm::dbgs() << Twine(llvm::divideCeil(totalSizeInBytes, 1024))
                          << " KiB of DMA buffers in fast memory space\n";);

  if (clFastMemoryCapacity && totalSizeInBytes > clFastMemoryCapacity) {
    // TODO(bondhugula): selecting the DMA depth so that the result DMA buffers
    // fit in fast memory is a TODO - not complex.
    forOp->emitError(
        "Total size of all DMA buffers' exceeds memory capacity\n");
  }
}

PassResult DmaGeneration::runOnFunction(Function *f) {
  FuncBuilder topBuilder(f);

  zeroIndex = topBuilder.create<ConstantIndexOp>(f->getLoc(), 0);

  for (auto &block : *f) {
    for (auto &inst : block) {
      if (auto forOp = cast<OperationInst>(inst).dyn_cast<AffineForOp>()) {
        runOnAffineForOp(forOp);
      }
    }
  }
  // This function never leaves the IR in an invalid state.
  return success();
}

static PassRegistration<DmaGeneration>
    pass("dma-generate", "Generate DMAs for memory operations");
