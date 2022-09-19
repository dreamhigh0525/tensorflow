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

#include <algorithm>
#include <memory>
#include <utility>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/transforms/legalize_to_linalg_utils.h"
#include "mlir-hlo/Dialect/mhlo/transforms/map_mhlo_to_scalar_op.h"
#include "mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "mlir-hlo/Dialect/mhlo/transforms/type_conversion.h"
#include "mlir-hlo/Dialect/thlo/IR/thlo_ops.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

namespace mlir {
namespace mhlo {

#define GEN_PASS_DEF_LEGALIZEMHLOTOTHLOPASS
#include "mlir-hlo/Dialect/mhlo/transforms/mhlo_passes.h.inc"

namespace {

bool isIotaArray(llvm::ArrayRef<int64_t> array, int expectedSize = -1) {
  if (expectedSize != -1 && static_cast<int>(array.size()) != expectedSize)
    return false;
  for (int64_t i = 0, e = array.size(); i < e; ++i) {
    if (i != array[i]) return false;
  }
  return true;
}

struct ConcatenateOpPattern : public OpConversionPattern<mhlo::ConcatenateOp> {
  using OpConversionPattern<mhlo::ConcatenateOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ConcatenateOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    const int64_t concatDim = op.getDimension();
    const Location loc = op.getLoc();
    const Value anyOperand = adaptor.getVal().front();

    auto resultTy = typeConverter->convertType(op.getResult().getType())
                        .cast<RankedTensorType>();
    const ArrayRef<int64_t> resultShape = resultTy.getShape();
    const int64_t rank = resultTy.getRank();

    // Determine init tensor size.
    SmallVector<int64_t> staticInitSizes(resultShape.begin(),
                                         resultShape.end());
    SmallVector<Value> dynamicInitSizes;
    for (int64_t i = 0; i < rank; ++i) {
      // No need to materialize anything for static dimensions.
      if (staticInitSizes[i] != ShapedType::kDynamicSize) {
        continue;
      }

      // For all dimensions other than the concatenation dimension, we can copy
      // the size from any operand.
      if (i != static_cast<int64_t>(concatDim)) {
        dynamicInitSizes.push_back(
            rewriter.create<tensor::DimOp>(loc, anyOperand, i));
        continue;
      }

      // For the concatenation dimensions, sum up the sizes of all operands in
      // that dimension.
      int64_t staticSum = 0;
      Value dynamicSum;
      for (const Value operand : adaptor.getVal()) {
        auto operandTy = operand.getType().cast<RankedTensorType>();
        if (operandTy.getDimSize(concatDim) == ShapedType::kDynamicSize) {
          const Value dynamicSummand =
              rewriter.create<tensor::DimOp>(loc, operand, concatDim);
          if (dynamicSum) {
            dynamicSum =
                rewriter.create<arith::AddIOp>(loc, dynamicSum, dynamicSummand);
          } else {
            dynamicSum = dynamicSummand;
          }
        } else {
          staticSum += operandTy.getDimSize(concatDim);
        }
      }
      assert(dynamicSum && "expect at least one dynamic summand in this case");
      if (staticSum != 0) {
        dynamicSum = rewriter.create<arith::AddIOp>(
            loc, dynamicSum,
            rewriter.create<arith::ConstantIndexOp>(loc, staticSum));
      }
      dynamicInitSizes.push_back(dynamicSum);
    }

    // Create init tensor and the new concat op.
    auto init = rewriter.create<linalg::InitTensorOp>(
        loc, dynamicInitSizes, staticInitSizes, resultTy.getElementType());
    rewriter.replaceOpWithNewOp<thlo::ConcatenateOp>(
        op, resultTy, adaptor.getVal(), init, concatDim);
    return success();
  }
};

struct DynamicBroadcastInDimOpPattern
    : public OpConversionPattern<mhlo::DynamicBroadcastInDimOp> {
  using OpConversionPattern<mhlo::DynamicBroadcastInDimOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::DynamicBroadcastInDimOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    auto loc = op.getLoc();
    Value outputDimensions = adaptor.getOutputDimensions();
    auto operandTy = adaptor.getOperand().getType().cast<RankedTensorType>();
    auto resultTy =
        typeConverter->convertType(op.getType()).cast<RankedTensorType>();

    // Only  apply to broadcasts that cannot be lowered to linalg, i.e. those
    // for which we do not know their expansion behavior at compile time.
    int64_t countKnownExpansionBehavior = 0;
    if (auto expandingDims = op.getKnownExpandingDimensions()) {
      countKnownExpansionBehavior += expandingDims->size();
    }
    if (auto nonexpandingDims = op.getKnownNonexpandingDimensions()) {
      countKnownExpansionBehavior += nonexpandingDims->size();
    }
    if (operandTy.getRank() == countKnownExpansionBehavior) return failure();

    // Create init tensor as none of the operands are reusable/updatable.
    SmallVector<Value> dynamicDims;
    SmallVector<int64_t> staticShapeInfo;
    for (int i = 0; i < resultTy.getRank(); i++) {
      dynamicDims.push_back(rewriter.create<tensor::ExtractOp>(
          loc, outputDimensions,
          ValueRange{rewriter.create<arith::ConstantIndexOp>(loc, i)}));
      staticShapeInfo.push_back(ShapedType::kDynamicSize);
    }
    auto initTensor = rewriter.create<linalg::InitTensorOp>(
        loc, dynamicDims, staticShapeInfo, resultTy.getElementType());

    auto broadcastDims = rewriter.getDenseI64ArrayAttr(
        llvm::to_vector(op.getBroadcastDimensions().getValues<int64_t>()));

    DenseI64ArrayAttr knownExpandingDims;
    if (op.getKnownExpandingDimensions().has_value()) {
      knownExpandingDims = rewriter.getDenseI64ArrayAttr(llvm::to_vector(
          op.getKnownExpandingDimensionsAttr().getValues<int64_t>()));
    }
    DenseI64ArrayAttr knownNonexpandingDims;
    if (op.getKnownNonexpandingDimensions().has_value()) {
      knownNonexpandingDims = rewriter.getDenseI64ArrayAttr(llvm::to_vector(
          op.getKnownNonexpandingDimensionsAttr().getValues<int64_t>()));
    }

    rewriter.replaceOpWithNewOp<thlo::DynamicBroadcastInDimOp>(
        op, resultTy, adaptor.getOperand(), initTensor, broadcastDims,
        knownExpandingDims, knownNonexpandingDims);
    return success();
  }
};

// Rewrites simple gather patterns (as checked below).
struct GatherPattern : public OpConversionPattern<mhlo::GatherOp> {
  using OpConversionPattern<mhlo::GatherOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::GatherOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    auto startIndicesType =
        adaptor.getStartIndices().getType().dyn_cast<RankedTensorType>();
    auto operandType =
        adaptor.getOperand().getType().dyn_cast<RankedTensorType>();

    if (!startIndicesType || !operandType) return failure();

    // index_vector_dim must be the last dimension of start_indices.
    int indexVectorDim = op.getDimensionNumbers().getIndexVectorDim();
    if (startIndicesType.getRank() - 1 != indexVectorDim) return failure();

    // All slice_sizes must be 1.
    if (!llvm::all_of(op.getSliceSizes(), [](auto size) { return size == 1; }))
      return failure();

    // offset_dims must be []
    if (!op.getDimensionNumbers().getOffsetDims().empty()) return failure();

    // collapsed_slice_dims[] must be range(operand.rank)
    auto collapsedSliceDims = op.getDimensionNumbers().getCollapsedSliceDims();
    if (!isIotaArray(collapsedSliceDims, operandType.getRank()))
      return failure();

    // start_index_map[] must be range(start_indices.shape[index_vector_dim])
    auto startIndexMap = op.getDimensionNumbers().getStartIndexMap();
    if (!isIotaArray(startIndexMap,
                     startIndicesType.getShape()[indexVectorDim]))
      return failure();

    // The shape of the result must be statically known.
    auto resultType =
        typeConverter->convertType(op.getType()).cast<RankedTensorType>();
    if (resultType.getNumDynamicDims() > 0) return failure();

    auto loc = op.getLoc();
    auto initTensor = rewriter.create<linalg::InitTensorOp>(
        loc, mlir::ValueRange{}, resultType.getShape(),
        resultType.getElementType());
    rewriter.replaceOpWithNewOp<thlo::GatherOp>(
        op, resultType, adaptor.getOperand(), adaptor.getStartIndices(),
        initTensor);
    return success();
  }
};

static SmallVector<Value, 8> getReduceOpInitTensorDynSizes(
    OpBuilder& b, Location loc, Value operand, int64_t srcRank,
    RankedTensorType resultType, ArrayRef<int64_t> reductionDims) {
  SmallVector<Value, 8> dynShape;
  for (size_t i = 0, j = 0; i < srcRank; ++i) {
    if (j < reductionDims.size() && reductionDims[j] == i) {
      ++j;
      continue;
    }
    size_t resultIndex = i - j;
    if (!resultType.isDynamicDim(resultIndex)) continue;
    dynShape.push_back(b.create<tensor::DimOp>(loc, operand, resultIndex));
  }
  return dynShape;
}

struct ReductionPattern : public OpConversionPattern<mhlo::ReduceOp> {
  using OpConversionPattern<mhlo::ReduceOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ReduceOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto srcRank =
        adaptor.operands()[0].getType().cast<RankedTensorType>().getRank();
    auto reductionDims =
        llvm::to_vector(op.getDimensions().getValues<int64_t>());
    // mhlo.reduce doesn't specify the order of the reduction dimensions.
    std::sort(reductionDims.begin(), reductionDims.end());

    auto toRankedTensor = [](Value v) -> RankedTensorType {
      return v.getType().dyn_cast<RankedTensorType>();
    };

    SmallVector<Value> outputs;
    SmallVector<RankedTensorType> operandTypes, initTypes;
    SmallVector<Type> resultTypes;
    if (failed(typeConverter->convertTypes(op.getResultTypes(), resultTypes)))
      return failure();

    Location loc = op.getLoc();
    for (auto [operand, initValue, resultType] :
         llvm::zip(adaptor.operands(), adaptor.getInitValues(), resultTypes)) {
      auto initType = toRankedTensor(initValue);
      if (!initType)
        return rewriter.notifyMatchFailure(op,
                                           "expects known-rank init values");
      initTypes.push_back(initType);
      auto operandType = toRankedTensor(initValue);
      if (!operandType)
        return rewriter.notifyMatchFailure(op, "expects known-rank operands");
      operandTypes.push_back(operandType);
      initValue = rewriter.createOrFold<tensor::ExtractOp>(loc, initValue);
      auto tensorResultType = resultType.cast<RankedTensorType>();

      SmallVector<Value, 8> dynShape = getReduceOpInitTensorDynSizes(
          rewriter, loc, operand, srcRank, tensorResultType, reductionDims);
      Value initTensor = rewriter.create<linalg::InitTensorOp>(
          loc, dynShape, tensorResultType.getShape(),
          tensorResultType.getElementType());
      Value filledTensor =
          rewriter.create<linalg::FillOp>(loc, initValue, initTensor).result();
      outputs.push_back(filledTensor);
    }

    auto thloReduction = rewriter.create<thlo::ReductionOp>(
        loc, resultTypes, adaptor.operands(), outputs,
        rewriter.getDenseI64ArrayAttr(reductionDims));
    Region& region = thloReduction.combiner();
    rewriter.inlineRegionBefore(op.getBody(), region, region.end());

    // Convert the signature of the body. The reduce op 'computation' region
    // apply function has a signature with tensor types, this is converted to a
    // function with element types. E.g. the signature "(tensor<f32>,
    // tensor<f32>) -> tensor<f32>" will be converted to "(f32, f32) -> f32".
    // Also, we need to swap the operands of the function. The mhlo.reduce op
    // expects the init values to be the first parameters of the apply function,
    // while the thlo.reduction op expects the init values as the last
    // parameters of the 'combiner' region apply function.
    TypeConverter::SignatureConversion signatureConverter(
        thloReduction.getNumInputs() * 2);
    assert(thloReduction.getNumInputs() == thloReduction.getNumOutputs());
    for (const auto& [idx, val] : llvm::enumerate(operandTypes)) {
      signatureConverter.addInputs(
          /*origInputNo=*/idx + thloReduction.getNumInputs(),
          // type for new operand number 'idx'.
          typeConverter->convertType(val.getElementType()));
    }
    for (const auto& [idx, val] : llvm::enumerate(initTypes)) {
      signatureConverter.addInputs(
          /*origInputNo=*/idx,
          // type for new operand number 'idx' + thloReduction.getNumInputs()
          typeConverter->convertType(val.getElementType()));
    }
    rewriter.applySignatureConversion(&region, signatureConverter,
                                      getTypeConverter());

    rewriter.replaceOp(op, thloReduction.getResults());
    return success();
  }
};

bool isInBodyOfThloOp(Operation* op) {
  auto* parentOp = op->getParentRegion()->getParentOp();
  return isa<thlo::MapOp>(*parentOp) || isa<thlo::ReductionOp>(*parentOp) ||
         isa<thlo::ScatterOp>(*parentOp);
}

// Rewrites a mhlo::ReturnOp inside a thlo::ReductionOp to thlo::YieldOp.
struct ThloRegionReturnOpConversion
    : public OpConversionPattern<mhlo::ReturnOp> {
  using OpConversionPattern<mhlo::ReturnOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ReturnOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!isInBodyOfThloOp(op)) return failure();
    SmallVector<Value, 4> operands(adaptor.getOperands());
    auto loc = op.getLoc();
    for (size_t i = 0; i < operands.size(); ++i) {
      if (operands[i].getType().isa<ShapedType>()) {
        operands[i] = rewriter.create<tensor::ExtractOp>(loc, operands[i]);
      }
    }
    rewriter.replaceOpWithNewOp<thlo::YieldOp>(op, operands);
    return success();
  }
};

// Rewrites simple scatter patterns.
struct ScatterPattern : public OpConversionPattern<mhlo::ScatterOp> {
  using OpConversionPattern<mhlo::ScatterOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ScatterOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    // The variadic case is not supported.
    if (op.getUpdates().size() != 1) return failure();

    const auto& dims = op.getScatterDimensionNumbers();
    auto scatterIndicesType =
        adaptor.getScatterIndices().getType().dyn_cast<RankedTensorType>();
    if (!scatterIndicesType) return failure();

    // Only point updates are supported.
    //  - update_window_dims is []
    //  - inserted_window_dims is range(operand.shape.rank)
    //  - scatter_dims_to_operand_dims is range(scatter_indices.shape.rank)
    //  - index_vector_dim is scatter_indices.shape.rank-1
    if (!dims.getUpdateWindowDims().empty() ||
        !isIotaArray(dims.getInsertedWindowDims()) ||
        !isIotaArray(dims.getScatterDimsToOperandDims()) ||
        dims.getIndexVectorDim() != scatterIndicesType.getRank() - 1)
      return failure();

    auto opType =
        typeConverter->convertType(op.getType(0)).dyn_cast<ShapedType>();
    if (!opType)
      return failure();  // Type is a tensor in the non-variadic case.

    Location loc = op.getLoc();
    auto thloScatter = rewriter.create<thlo::ScatterOp>(
        loc, opType, adaptor.getScatterIndices(), adaptor.updates().front(),
        adaptor.operands().front());

    Region& region = thloScatter.update_computation();
    rewriter.inlineRegionBefore(op.getRegion(), region, region.end());

    // Convert the signature of the body by inserting
    // tensor.from_elements/tensor.extract.
    TypeConverter::SignatureConversion signatureConverter(2);
    for (const auto& [idx, val] :
         llvm::enumerate(thloScatter.update_computation().getArgumentTypes())) {
      signatureConverter.addInputs(
          idx, typeConverter->convertType(
                   val.cast<RankedTensorType>().getElementType()));
    }
    rewriter.applySignatureConversion(&region, signatureConverter,
                                      getTypeConverter());

    rewriter.replaceOp(op, thloScatter.getResults());
    return success();
  }
};

struct MapPattern : public OpConversionPattern<mhlo::MapOp> {
  using OpConversionPattern<mhlo::MapOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::MapOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto resultTy = typeConverter->convertType(op.getType()).cast<ShapedType>();
    assert(op.dimensions().size() == resultTy.getRank() &&
           "Expected a pointwise map");

    Location loc = op.getLoc();
    Value initTensor =
        getInitTensorFor(rewriter, loc, resultTy, op, adaptor.getOperands());

    auto thloMap = rewriter.create<thlo::MapOp>(loc, resultTy,
                                                adaptor.operands(), initTensor);
    Region& region = thloMap.mapper();
    rewriter.inlineRegionBefore(op.computation(), region, region.end());

    TypeConverter::SignatureConversion signatureConverter(
        thloMap.getNumInputs());
    for (const auto& [idx, val] : llvm::enumerate(thloMap.getInputs())) {
      signatureConverter.addInputs(
          idx,
          typeConverter->convertType(
              val.getType().dyn_cast<RankedTensorType>().getElementType()));
    }
    rewriter.applySignatureConversion(&region, signatureConverter,
                                      getTypeConverter());

    rewriter.replaceOp(op, thloMap.getResult());
    return success();
  }
};

/// Converts a HLO operation to a thlo.map op that contains the corresponding
/// scalar operations.
template <typename OpTy>
class PointwiseToTHLOConverter : public OpConversionPattern<OpTy> {
 public:
  using OpConversionPattern<OpTy>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      OpTy op, typename OpTy::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto getRank = [](Value v) {
      return v.getType().cast<ShapedType>().getRank();
    };
    int64_t maxRank = getRank(adaptor.getOperands().front());

    // Apply only if all operands have the same rank.
    if (!llvm::all_of(adaptor.getOperands(),
                      [&](Value v) { return getRank(v) == maxRank; })) {
      return rewriter.notifyMatchFailure(op,
                                         "Operands must have the same rank.");
    }

    // Find result type, if on tensors.
    Optional<ShapedType> resultTy;
    resultTy = this->typeConverter->convertType(op->getResultTypes().front())
                   .template dyn_cast<ShapedType>();

    // Check result type compatibility.
    if (!resultTy || !resultTy->hasRank() || resultTy->getRank() != maxRank ||
        !(resultTy->getElementType().isSignlessIntOrFloat() ||
          resultTy->getElementType().isa<ComplexType>())) {
      return rewriter.notifyMatchFailure(
          op, "mismatched operand/result types or iterator count");
    }

    auto loc = op.getLoc();
    // Within a thlo.map region, we can immediately de-tensorsize if the
    // computation is scalar. We do not do this on the top-level, as that would
    // break the nice invariant that all programs are exclusively on tensors,
    // which is currently relied on for fusion in some pipelines.
    if (maxRank == 0 && isInBodyOfTHLOOps(op)) {
      SmallVector<Value> inputs;
      for (auto input : adaptor.getOperands()) {
        inputs.push_back(
            rewriter.create<tensor::ExtractOp>(loc, input, ValueRange()));
      }
      Value scalarResult = mhlo::MhloOpToStdScalarOp::mapOp(
          op, resultTy->getElementType(), inputs, &rewriter);
      if (!scalarResult) return failure();
      rewriter.replaceOpWithNewOp<tensor::FromElementsOp>(op, *resultTy,
                                                          scalarResult);
      return success();
    }

    // Find input/output values and types.
    ValueRange inputs = adaptor.getOperands();
    Value initTensor =
        getInitTensorFor(rewriter, loc, *resultTy, op, adaptor.getOperands());

    auto mapOp = rewriter.create<thlo::MapOp>(loc, op->getResultTypes().front(),
                                              inputs, initTensor);

    {
      OpBuilder::InsertionGuard guard(rewriter);
      auto& region = mapOp.getRegion();

      SmallVector<Type, 4> blockArgTypes;
      SmallVector<Location, 4> blockArgLocs;
      for (Value v : inputs) {
        blockArgTypes.push_back(getElementTypeOrSelf(v));
        blockArgLocs.push_back(v.getLoc());
      }
      Block* block = rewriter.createBlock(&region, region.end(), blockArgTypes,
                                          blockArgLocs);

      Value innerResult =
          mhlo::MhloOpToStdScalarOp::mapOp(op, getElementTypeOrSelf(initTensor),
                                           block->getArguments(), &rewriter);
      rewriter.create<thlo::YieldOp>(loc, innerResult);
    }

    rewriter.replaceOp(op, mapOp->getResults());

    return success();
  }

 private:
  static bool isInBodyOfTHLOOps(Operation* op) {
    auto* parentOp = op->getParentRegion()->getParentOp();
    return parentOp->getDialect() ==
           parentOp->getContext()->getLoadedDialect<thlo::THLODialect>();
  }
};

class LegalizeMHLOToTHLOPass
    : public impl::LegalizeMHLOToTHLOPassBase<LegalizeMHLOToTHLOPass> {
  void runOnOperation() final {
    MLIRContext* ctx = &getContext();
    RewritePatternSet patterns(ctx);
    ConversionTarget target(*ctx);
    // clang-format off
    target.addLegalDialect<
        arith::ArithmeticDialect,
        complex::ComplexDialect,
        linalg::LinalgDialect,
        math::MathDialect,
        shape::ShapeDialect,
        tensor::TensorDialect,
        thlo::THLODialect>();
    // clang-format on
    target.addLegalOp<UnrealizedConversionCastOp>();

    auto typeConverter = std::make_unique<LinalgTypeConverter>();

    // List of patterns.
    // clang-format off
    patterns.insert<
        ConcatenateOpPattern,
        DynamicBroadcastInDimOpPattern,
        GatherPattern,
        ScatterPattern,
        ThloRegionReturnOpConversion>(*typeConverter, ctx);
    // clang-format on

    if (enableExperimental) {
      // clang-format off
      patterns.insert<
          ReductionPattern,
          MapPattern,
          PointwiseToTHLOConverter<mhlo::AbsOp>,
          PointwiseToTHLOConverter<mhlo::AddOp>,
          PointwiseToTHLOConverter<mhlo::AndOp>,
          PointwiseToTHLOConverter<mhlo::Atan2Op>,
          PointwiseToTHLOConverter<mhlo::BitcastConvertOp>,
          PointwiseToTHLOConverter<mhlo::CbrtOp>,
          PointwiseToTHLOConverter<mhlo::CeilOp>,
          PointwiseToTHLOConverter<mhlo::ClampOp>,
          PointwiseToTHLOConverter<mhlo::ClzOp>,
          PointwiseToTHLOConverter<mhlo::CompareOp>,
          PointwiseToTHLOConverter<mhlo::ComplexOp>,
          PointwiseToTHLOConverter<mhlo::ConvertOp>,
          PointwiseToTHLOConverter<mhlo::CopyOp>,
          PointwiseToTHLOConverter<mhlo::CosineOp>,
          PointwiseToTHLOConverter<mhlo::DivOp>,
          PointwiseToTHLOConverter<mhlo::ExpOp>,
          PointwiseToTHLOConverter<mhlo::Expm1Op>,
          PointwiseToTHLOConverter<mhlo::FloorOp>,
          PointwiseToTHLOConverter<mhlo::ImagOp>,
          PointwiseToTHLOConverter<mhlo::IsFiniteOp>,
          PointwiseToTHLOConverter<mhlo::LogOp>,
          PointwiseToTHLOConverter<mhlo::LogisticOp>,
          PointwiseToTHLOConverter<mhlo::Log1pOp>,
          PointwiseToTHLOConverter<mhlo::MaxOp>,
          PointwiseToTHLOConverter<mhlo::MinOp>,
          PointwiseToTHLOConverter<mhlo::MulOp>,
          PointwiseToTHLOConverter<mhlo::NegOp>,
          PointwiseToTHLOConverter<mhlo::NotOp>,
          PointwiseToTHLOConverter<mhlo::OrOp>,
          PointwiseToTHLOConverter<mhlo::PopulationCountOp>,
          PointwiseToTHLOConverter<mhlo::PowOp>,
          PointwiseToTHLOConverter<mhlo::RealOp>,
          PointwiseToTHLOConverter<mhlo::RemOp>,
          PointwiseToTHLOConverter<mhlo::RoundNearestEvenOp>,
          PointwiseToTHLOConverter<mhlo::RoundOp>,
          PointwiseToTHLOConverter<mhlo::RsqrtOp>,
          PointwiseToTHLOConverter<mhlo::ShiftLeftOp>,
          PointwiseToTHLOConverter<mhlo::ShiftRightArithmeticOp>,
          PointwiseToTHLOConverter<mhlo::ShiftRightLogicalOp>,
          PointwiseToTHLOConverter<mhlo::SignOp>,
          PointwiseToTHLOConverter<mhlo::SineOp>,
          PointwiseToTHLOConverter<mhlo::SqrtOp>,
          PointwiseToTHLOConverter<mhlo::SubtractOp>,
          PointwiseToTHLOConverter<mhlo::TanhOp>,
          PointwiseToTHLOConverter<mhlo::XorOp>,
          PointwiseToTHLOConverter<mhlo::ReducePrecisionOp>,
          ThloRegionReturnOpConversion>(*typeConverter, ctx);
      // clang-format on
    }

    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns)))) {
      return signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>> createLegalizeMHLOToTHLOPass() {
  return std::make_unique<LegalizeMHLOToTHLOPass>();
}

}  // namespace mhlo
}  // namespace mlir
