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

// This file implements logic for lowering HLO dialect to LHLO dialect.

#include <functional>
#include <memory>
#include <utility>

#include "mlir-hlo/Dialect/lhlo/IR/lhlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/transforms/bufferizable_op_interface_impl.h"
#include "mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Bufferization/Transforms/Bufferize.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Value.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
namespace mhlo {

#define GEN_PASS_DEF_HLOLEGALIZETOMEMREFPASS
#include "mlir-hlo/Dialect/mhlo/transforms/mhlo_passes.h.inc"

namespace {

using bufferization::AnalysisState;
using bufferization::BufferizableOpInterface;
using bufferization::BufferizationOptions;
using bufferization::BufferRelation;
using bufferization::replaceOpWithNewBufferizedOp;

struct CustomCallOpInterface
    : public BufferizableOpInterface::ExternalModel<CustomCallOpInterface,
                                                    mhlo::CustomCallOp> {
  bool bufferizesToMemoryRead(Operation *, OpOperand &,
                              const AnalysisState &) const {
    return true;
  }

  bool bufferizesToMemoryWrite(Operation *, OpOperand &,
                               const AnalysisState &) const {
    return false;  // Arguments are read-only.
  }

  SmallVector<OpResult> getAliasingOpResult(Operation *, OpOperand &,
                                            const AnalysisState &) const {
    return {};
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    auto customCallOp = cast<mhlo::CustomCallOp>(op);

    // Bufferize arguments.
    SmallVector<Value> bufferArgs;
    for (OpOperand &operand : customCallOp->getOpOperands()) {
      if (!operand.get().getType().isa<TensorType>()) return failure();
      FailureOr<Value> operandBuffer =
          getBuffer(rewriter, operand.get(), options);
      if (failed(operandBuffer)) return failure();
      bufferArgs.push_back(*operandBuffer);
    }

    // Allocate outputs.
    for (OpResult result : customCallOp->getOpResults()) {
      auto tensorType = result.getType().cast<RankedTensorType>();
      if (!tensorType) return failure();
      // TODO(springerm): Create alloc_tensor ops during TensorCopyInsertion.
      AnalysisState analysisState(options);
      FailureOr<Value> tensorAlloc =
          bufferization::allocateTensorForShapedValue(
              rewriter, op->getLoc(), result,
              analysisState.isTensorYielded(result), options);
      if (failed(tensorAlloc)) return failure();
      auto memrefType =
          MemRefType::get(tensorType.getShape(), tensorType.getElementType());
      Value resultBuffer = rewriter.create<bufferization::ToMemrefOp>(
          op->getLoc(), memrefType, *tensorAlloc);
      bufferArgs.push_back(resultBuffer);
    }

    auto lhloOp = rewriter.create<lmhlo::CustomCallOp>(
        op->getLoc(), llvm::None, bufferArgs, op->getAttrs());
    // lmhlo.custom_call uses a segment_size attribute to tell input from output
    // arguments.
    lhloOp->setAttr(lhloOp.getOperandSegmentSizeAttr(),
                    rewriter.getDenseI32ArrayAttr(
                        {static_cast<int32_t>(op->getNumOperands()),
                         static_cast<int32_t>(op->getNumResults())}));
    bufferization::replaceOpWithBufferizedValues(
        rewriter, op, makeArrayRef(bufferArgs).slice(op->getNumOperands()));
    return success();
  }
};

struct ReshapeOpInterface
    : public BufferizableOpInterface::ExternalModel<ReshapeOpInterface,
                                                    mhlo::ReshapeOp> {
  bool bufferizesToMemoryRead(Operation * /*op*/, OpOperand & /*opOperand*/,
                              const AnalysisState & /*state*/) const {
    return false;
  }

  bool bufferizesToMemoryWrite(Operation * /*op*/, OpOperand & /*opOperand*/,
                               const AnalysisState & /*state*/) const {
    return false;
  }

  SmallVector<OpResult> getAliasingOpResult(
      Operation *op, OpOperand & /*opOperand*/,
      const AnalysisState & /*state*/) const {
    return {op->getResult(0)};
  }

  BufferRelation bufferRelation(Operation * /*op*/, OpResult /*opResult*/,
                                const AnalysisState & /*state*/) const {
    return BufferRelation::Equivalent;
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    auto reshapeOp = cast<mhlo::ReshapeOp>(op);
    auto unrankedOperandType =
        reshapeOp.getOperand().getType().dyn_cast<UnrankedTensorType>();
    if (unrankedOperandType == nullptr) return success();

    // The buffer still has the old (pre-reshape) type.
    FailureOr<Value> operandBuffer =
        getBuffer(rewriter, reshapeOp.getOperand(), options);
    if (failed(operandBuffer)) return failure();

    auto resultType = reshapeOp.getType().cast<RankedTensorType>();
    auto destType =
        MemRefType::get(resultType.getShape(), resultType.getElementType());
    replaceOpWithNewBufferizedOp<memref::CastOp>(rewriter, op, destType,
                                                 *operandBuffer);
    return success();
  }
};

struct DynamicReshapeOpInterface
    : public BufferizableOpInterface::ExternalModel<DynamicReshapeOpInterface,
                                                    mhlo::DynamicReshapeOp> {
  bool bufferizesToMemoryRead(Operation * /*op*/, OpOperand & /*opOperand*/,
                              const AnalysisState & /*state*/) const {
    return false;
  }

  bool bufferizesToMemoryWrite(Operation * /*op*/, OpOperand & /*opOperand*/,
                               const AnalysisState & /*state*/) const {
    return false;
  }

  SmallVector<OpResult> getAliasingOpResult(
      Operation *op, OpOperand & /*opOperand*/,
      const AnalysisState & /*state*/) const {
    return {op->getResult(0)};
  }

  BufferRelation bufferRelation(Operation * /*op*/, OpResult /*opResult*/,
                                const AnalysisState & /*state*/) const {
    return BufferRelation::Equivalent;
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    auto reshapeOp = cast<mhlo::DynamicReshapeOp>(op);

    // The buffer still has the old (pre-reshape) type.
    FailureOr<Value> operandBuffer =
        getBuffer(rewriter, reshapeOp.getOperand(), options);
    FailureOr<Value> outputShapeBuffer =
        getBuffer(rewriter, reshapeOp.getOutputShape(), options);
    if (failed(operandBuffer) || failed(outputShapeBuffer)) return failure();

    ShapedType resultType;
    TensorType opResultType = reshapeOp.getType();
    if (auto rankedType = opResultType.dyn_cast<RankedTensorType>()) {
      resultType =
          MemRefType::get(rankedType.getShape(), rankedType.getElementType());
    } else if (auto unrankedType =
                   opResultType.dyn_cast<UnrankedTensorType>()) {
      resultType = UnrankedMemRefType::get(unrankedType.getElementType(), 0);
    }
    auto operand = *operandBuffer;
    // If the operand has a non-identity affine map, we will have to add a copy.
    auto bufferType = operandBuffer->getType().dyn_cast<MemRefType>();
    if (bufferType && !bufferType.getLayout().isIdentity()) {
      // TODO(springerm): Create alloc_tensor ops during TensorCopyInsertion.
      AnalysisState analysisState(options);
      FailureOr<Value> tensorAlloc =
          bufferization::allocateTensorForShapedValue(
              rewriter, op->getLoc(), *operandBuffer,
              analysisState.isTensorYielded(reshapeOp.getResult()), options);
      if (failed(tensorAlloc)) return failure();
      auto memrefType =
          MemRefType::get(bufferType.getShape(), bufferType.getElementType());
      operand = rewriter.create<bufferization::ToMemrefOp>(
          op->getLoc(), memrefType, *tensorAlloc);
    }
    bufferization::replaceOpWithNewBufferizedOp<memref::ReshapeOp>(
        rewriter, op, resultType, operand, *outputShapeBuffer);
    return success();
  }
};

// Inserts dynamic memref to change the layout of the memref to put 0-stride
// and size of the target dimension if size-1 dimension expansion is
// necessary.
FailureOr<Value> insertDynamicMemrefCastOp(
    mhlo::DynamicBroadcastInDimOp op, Value operand, RewriterBase &rewriter,
    const BufferizationOptions &options) {
  auto loc = op.getLoc();
  auto operandType = operand.getType().cast<MemRefType>();
  auto operandShape = operandType.getShape();
  auto operandRank = operandType.getRank();

  auto resultType = op.getType().cast<RankedTensorType>();
  auto resultRank = resultType.getRank();

  Value zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);
  Value one = rewriter.create<arith::ConstantIndexOp>(loc, 1);

  // Compute a reversed scan product. Compute the stride for the dimensions so
  // far, working from minor to major dimensions. Additionally, save the
  // operand shape Values to use in the next loop.
  SmallVector<Value, 2> operandStrides(operandRank, one);
  SmallVector<Value, 2> operandSizes(operandRank, one);
  Value strideSoFar = one;
  for (int i = operandRank - 1; i >= 0; --i) {
    Value operandDimSize =
        ShapedType::isDynamic(operandShape[i])
            ? rewriter.create<memref::DimOp>(loc, operand, i).getResult()
            : rewriter.create<arith::ConstantIndexOp>(loc, operandShape[i])
                  .getResult();
    operandSizes[i] = operandDimSize;

    operandStrides[i] = strideSoFar;
    if (i > 0) {
      strideSoFar =
          rewriter.create<arith::MulIOp>(loc, strideSoFar, operandDimSize);
    }
  }

  SmallVector<OpFoldResult, 2> sizes, strides;
  sizes.reserve(resultRank);
  strides.reserve(resultRank);

  DenseMap<int, int> outputToInputDim;
  for (const auto &dim : llvm::enumerate(op.getBroadcastDimensions())) {
    outputToInputDim[dim.value().getSExtValue()] = dim.index();
  }
  for (int i = 0; i < resultRank; ++i) {
    Value iVal = rewriter.create<arith::ConstantIndexOp>(loc, i);
    FailureOr<Value> outputDimsBuffer =
        getBuffer(rewriter, op.getOutputDimensions(), options);
    if (failed(outputDimsBuffer)) return failure();
    Value resultDimSize =
        rewriter.create<memref::LoadOp>(loc, *outputDimsBuffer, iVal);
    if (!resultDimSize.getType().isIndex()) {
      resultDimSize = rewriter.create<arith::IndexCastOp>(
          loc, rewriter.getIndexType(), resultDimSize);
    }
    if (resultType.isDynamicDim(i)) {
      sizes.push_back(resultDimSize);
    } else {
      sizes.push_back(rewriter.getIndexAttr(resultType.getDimSize(i)));
    }

    auto it = outputToInputDim.find(i);
    // If the rank of the output is greater than the rank of the input, i.e.
    // there was no output dimension in the inverse broadcast_dimensions map
    // we also set stride to 0 to emulate padding of the shape with 1s and the
    // corresponding expansion.
    if (it == outputToInputDim.end()) {
      strides.push_back(zero);
      continue;
    }

    // There can be two cases:
    // 1) Operand dim == result dim => expansion is not needed
    //    => stride flattened buffer stride
    // 2) Operand dim < result dim => expansion is needed => stride := 0.
    int dim = it->second;
    Value isExpansion = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::slt, operandSizes[dim], resultDimSize);
    Value select = rewriter.create<mlir::arith::SelectOp>(
        loc, isExpansion, zero, operandStrides[dim]);
    strides.push_back(select);
  }

  // Type-erased memref type with static rank and dynamic strides.
  SmallVector<int64_t, 2> dynamicLayout(resultRank,
                                        ShapedType::kDynamicStrideOrOffset);
  auto typeErasedMemrefType = MemRefType::get(
      resultType.getShape(), operandType.getElementType(),
      makeStridedLinearLayoutMap(dynamicLayout,
                                 /*offset=*/0, rewriter.getContext()));

  auto transformedOperand = rewriter.create<memref::ReinterpretCastOp>(
      loc, typeErasedMemrefType, operand,
      /*offset=*/rewriter.getI64IntegerAttr(0), sizes, strides);
  return transformedOperand.getResult();
}

struct DynamicBroadcastInDimOpInterface
    : public BufferizableOpInterface::ExternalModel<
          DynamicBroadcastInDimOpInterface, mhlo::DynamicBroadcastInDimOp> {
  bool bufferizesToMemoryRead(Operation * /*op*/, OpOperand & /*opOperand*/,
                              const AnalysisState & /*state*/) const {
    return true;
  }

  bool bufferizesToMemoryWrite(Operation * /*op*/, OpOperand & /*opOperand*/,
                               const AnalysisState & /*state*/) const {
    return false;
  }

  SmallVector<OpResult> getAliasingOpResult(
      Operation *op, OpOperand & /*opOperand*/,
      const AnalysisState & /*state*/) const {
    return {op->getResult(0)};
  }

  BufferRelation bufferRelation(Operation * /*op*/, OpResult /*opResult*/,
                                const AnalysisState & /*state*/) const {
    // The op may allocate.
    return BufferRelation::None;
  }

  LogicalResult bufferize(Operation *op, RewriterBase &rewriter,
                          const BufferizationOptions &options) const {
    auto broadcastInDimOp = cast<mhlo::DynamicBroadcastInDimOp>(op);
    auto resultType = broadcastInDimOp.getType().dyn_cast<RankedTensorType>();
    if (!resultType) return success();

    // The buffer still has the old (pre-reshape) type.
    FailureOr<Value> operandBuffer =
        getBuffer(rewriter, broadcastInDimOp.getOperand(), options);
    if (failed(operandBuffer)) return failure();
    FailureOr<Value> result = insertDynamicMemrefCastOp(
        broadcastInDimOp, *operandBuffer, rewriter, options);
    if (failed(result)) return failure();
    bufferization::replaceOpWithBufferizedValues(rewriter, op, *result);
    return success();
  }
};

struct HloLegalizeToMemrefPass
    : public impl::HloLegalizeToMemrefPassBase<HloLegalizeToMemrefPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<bufferization::BufferizationDialect, memref::MemRefDialect,
                    mhlo::MhloDialect, lmhlo::LmhloDialect>();
    registerBufferizableOpInterfaceExternalModels(registry);
  }

 public:
  void runOnOperation() override {
    bufferization::BufferizationOptions options =
        bufferization::getPartialBufferizationOptions();
    options.opFilter.allowDialect<mhlo::MhloDialect>();
    if (failed(bufferizeOp(getOperation(), options))) signalPassFailure();
  }
};

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> createLegalizeToMemrefPass() {
  return std::make_unique<HloLegalizeToMemrefPass>();
}

void registerBufferizableOpInterfaceExternalModels(DialectRegistry &registry) {
  registry.addExtension(+[](MLIRContext *ctx, MhloDialect * /*dialect*/) {
    CustomCallOp::attachInterface<CustomCallOpInterface>(*ctx);
    ReshapeOp::attachInterface<ReshapeOpInterface>(*ctx);
    DynamicReshapeOp::attachInterface<DynamicReshapeOpInterface>(*ctx);
    DynamicBroadcastInDimOp::attachInterface<DynamicBroadcastInDimOpInterface>(
        *ctx);
  });
}

}  // namespace mhlo
}  // namespace mlir
