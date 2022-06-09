/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

// This file implements logic for lowering HLO/LHLO dialect to Linalg dialect.

#include <algorithm>
#include <numeric>
#include <string>
#include <utility>

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops_base_attrs.h"
#include "mlir-hlo/Dialect/mhlo/transforms/PassDetail.h"
#include "mlir-hlo/Dialect/mhlo/transforms/map_mhlo_to_scalar_op.h"
#include "mlir-hlo/Dialect/mhlo/transforms/rewriters.h"
#include "mlir-hlo/Dialect/mhlo/transforms/type_conversion.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Arithmetic/Utils/Utils.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Complex/IR/Complex.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Utils/Utils.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace {

template <typename OpTy>
SmallVector<NamedAttribute> pruneAttributeList(OpTy op) {
  auto opAttributes = op.getAttributeNames();
  llvm::StringSet<> elidedAttrs;
  elidedAttrs.insert(opAttributes.begin(), opAttributes.end());
  SmallVector<NamedAttribute> preservedAttrs;
  for (auto attr : op->getAttrs()) {
    if (elidedAttrs.count(attr.getName())) continue;
    preservedAttrs.push_back(attr);
  }
  return preservedAttrs;
}

/// Returns an ArrayAttr that contains `nLoops` attributes. All the attributes
/// are "parallel" except the last `nReduction` elements, where are "reduction"
/// attributes.
SmallVector<StringRef, 3> getParallelAndReductionIterators(
    unsigned nLoops, unsigned nReduction) {
  SmallVector<StringRef, 3> res(nLoops - nReduction,
                                getParallelIteratorTypeName());
  res.append(nReduction, getReductionIteratorTypeName());
  return res;
}

SmallVector<StringRef, 3> getNParallelLoopsAttrs(unsigned nParallelLoops) {
  return getParallelAndReductionIterators(nParallelLoops, 0);
}

Value getResultValue(Operation* op) { return op->getResult(0); }

ShapedType getHloOpResultType(Operation* op) {
  return getResultValue(op).getType().cast<ShapedType>();
}

bool verifyHloOpBufferOrTensorSemantics(Operation* op) {
  auto verifyType = [&](Value val) -> bool {
    return val.getType().isa<RankedTensorType>();
  };
  if (!llvm::all_of(op->getOperands(), verifyType)) return false;
  return llvm::all_of(op->getResults(), verifyType);
}

Value getInitTensor(OpBuilder& b, Location loc, ShapedType type,
                    ArrayRef<Value> dynSizes) {
  return b.create<linalg::InitTensorOp>(loc, dynSizes, type.getShape(),
                                        type.getElementType());
}

Value getInitSparseTensor(OpBuilder& b, Location loc, ShapedType type,
                          ArrayRef<Value> dynSizes) {
  return b.create<bufferization::AllocTensorOp>(loc, type, dynSizes);
}

Value getInitTensorFor(OpBuilder& b, Location loc, ShapedType resultType,
                       Operation* op, ValueRange operands) {
  bool isSparse = sparse_tensor::getSparseTensorEncoding(resultType) != nullptr;
  // Collect the sizes for a ranked tensor to be passed as parameter to a
  // new tensor initialization operation. This operation only needs the
  // dynamic sizes.
  SmallVector<Value> sizes;
  if (resultType.hasRank() && !resultType.hasStaticShape()) {
    // Ask the op for its output shape.
    auto shapeSource = cast<InferShapedTypeOpInterface>(op);
    SmallVector<Value, 1> reifiedShapes;
    (void)shapeSource.reifyReturnTypeShapes(b, operands, reifiedShapes);
    assert(reifiedShapes.size() == 1 && "Expected one reified result");
    // Construct sizes for the required dimensions.
    for (auto& en : llvm::enumerate(resultType.getShape())) {
      if (en.value() != ShapedType::kDynamicSize) continue;
      sizes.push_back(b.create<tensor::ExtractOp>(
          loc, reifiedShapes[0],
          ValueRange{b.create<arith::ConstantIndexOp>(loc, en.index())}));
    }
  }
  return isSparse ? getInitSparseTensor(b, loc, resultType, sizes)
                  : getInitTensor(b, loc, resultType, sizes);
}

Value fillTensorWithZeros(OpBuilder& builder, Location loc, Value tensor) {
  auto type = tensor.getType().cast<ShapedType>();
  Value zero;
  // Complex numbers are a special case.
  if (auto complexType = type.getElementType().dyn_cast<ComplexType>()) {
    auto zeroElement = builder.getZeroAttr(complexType.getElementType());
    auto zeroAttr = builder.getArrayAttr({zeroElement, zeroElement});
    zero = builder.create<complex::ConstantOp>(loc, complexType, zeroAttr);
  } else {
    auto zeroAttr = builder.getZeroAttr(type.getElementType());
    zero = builder.create<arith::ConstantOp>(loc, zeroAttr);
  }
  return builder.create<linalg::FillOp>(loc, zero, tensor).result();
}

static inline bool hasIntegralShapeType(Operation* op) {
  auto stp = op->getOperand(0).getType().dyn_cast<ShapedType>();
  return stp && stp.getElementType().isIntOrIndex();
}

/// Sparsifies a (block of) operation(s) that cannot be handled directly
/// by the sparse compiler but has well-known semi-ring semantics.
///
/// This yields something of the following form:
///
///   %result = sparse_tensor.unary %values[0]
///     present={
///       ^bb1(%val):
///         ... codegen proceeds here using %val ....
///         sparse_tensor.yield
///     }
///     absent={}
///   linalg.yield %result
Value preSparsify(Operation* op, llvm::SmallVector<Value, 2>& values, Type rtp,
                  OpBuilder* b) {
  // Apply for semi-ring operations that lower to elaborate code
  // (any sign-op, any elt-wise conversion, or an integral abs-op).
  if (isa<mhlo::SignOp>(op) || isa<mhlo::ConvertOp>(op) ||
      (isa<mhlo::AbsOp>(op) && hasIntegralShapeType(op))) {
    if (!sparse_tensor::getSparseTensorEncoding(op->getResult(0).getType()) &&
        !sparse_tensor::getSparseTensorEncoding(op->getOperand(0).getType()))
      return Value();
    Location loc = op->getLoc();
    auto semiring = b->create<sparse_tensor::UnaryOp>(loc, rtp, values[0]);
    Type itp = values[0].getType();
    Block* present = b->createBlock(&semiring.presentRegion(), {}, itp, loc);
    b->setInsertionPointToStart(&semiring.presentRegion().front());
    values[0] = present->getArgument(0);
    return semiring;
  }
  return Value();
}

/// Finalizes sparse semi-ring construction.
Value postSparsify(Operation* op, Value semiring, Value result, OpBuilder* b) {
  if (semiring) {
    b->create<sparse_tensor::YieldOp>(op->getLoc(), result);
    b->setInsertionPointAfter(semiring.getDefiningOp());
    return semiring;
  }
  return result;
}

SmallVector<int64_t, 4> extract1DVector(DenseIntElementsAttr elements) {
  SmallVector<int64_t, 4> ret;
  for (const APInt& element : elements) {
    ret.push_back(element.getLimitedValue());
  }
  return ret;
}

/// Returns a permutation AffineMap that puts all reduction dimensions to the
/// last. The order of parallel loops and reduction loops are all sorted. E.g.,
/// if `rank` is 4 and `reductionDims` is {1, 3}, then
/// "(d0, d1, d2, d3) -> (d0, d2, d1, d3)" is used. The inverse permutation of
/// the AffineMap is returned.
AffineMap getTransposeMapForReduction(MLIRContext* context, int rank,
                                      ArrayRef<int64_t> reductionDims) {
  llvm::SmallSetVector<int, 4> s;
  for (auto dim : reductionDims) s.insert(dim);

  SmallVector<unsigned, 4> permutation;
  for (int i = 0; i < rank; ++i)
    if (!s.count(i)) permutation.push_back(i);
  for (auto dim : reductionDims) permutation.push_back(dim);

  auto map = AffineMap::getPermutationMap(permutation, context);
  return inversePermutation(map);
}

/// Returns true if the given `attr` is a splat of the given `value`.
bool isSplatValue(DenseIntElementsAttr attr, uint64_t value) {
  return attr.isSplat() && attr.getSplatValue<uint64_t>() == value;
}

/// Returns true if the given `dimensionNumbers` from a mhlo.convolution op
/// follows a canonical form:
///
/// * Input dimensions have order: (batch_count, spatial_dims,
///   input_channel_count).
/// * Filter dimensions have order: (spatial_dims, input_channel_count,
///   output_channel_count).
/// * Output dimensions have order: (batch_count, spatial_dims,
///   output_channel_count).
static bool hasCanonicalDimensionNumbers(
    mhlo::ConvDimensionNumbersAttr dimensionNumbers) {
  const int inputSpatialRank =
      llvm::size(dimensionNumbers.getInputSpatialDimensions());
  // The dimensions for input should follow the order of
  // batch_count, spatial_dims..., input_feature_count.
  if (dimensionNumbers.getInputBatchDimension() != 0 ||
      dimensionNumbers.getInputFeatureDimension() != (inputSpatialRank + 1)) {
    return false;
  }

  const int kernelSpatialRank =
      llvm::size(dimensionNumbers.getKernelSpatialDimensions());
  // The dimensions for filter should follow the order of
  // spatial_dims..., input_feature_count, num_output_feature_count.
  if (dimensionNumbers.getKernelInputFeatureDimension() != kernelSpatialRank ||
      dimensionNumbers.getKernelOutputFeatureDimension() !=
          (kernelSpatialRank + 1)) {
    return false;
  }

  const int outputSpatialRank =
      llvm::size(dimensionNumbers.getOutputSpatialDimensions());
  // The dimensions for output should follow the order of
  // batch_count, spatial_dims.., output_feature_count.
  if (dimensionNumbers.getOutputBatchDimension() != 0 ||
      dimensionNumbers.getOutputFeatureDimension() != (outputSpatialRank + 1)) {
    return false;
  }

  if (inputSpatialRank != outputSpatialRank ||
      inputSpatialRank != kernelSpatialRank) {
    return false;
  }

  const auto* inputSpatialDim =
      dimensionNumbers.getInputSpatialDimensions().begin();
  const auto* kernelSpatialDim =
      dimensionNumbers.getKernelSpatialDimensions().begin();
  const auto* outputSpatialDim =
      dimensionNumbers.getOutputSpatialDimensions().begin();
  // Check spatial dims are ordered correctly.
  for (int i = 0; i < inputSpatialRank; ++i) {
    const int dim = i + 1;
    if ((*inputSpatialDim++) != dim || (*outputSpatialDim++) != dim ||
        (*kernelSpatialDim++) != i) {
      return false;
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
// mhlo.RngUniformOp conversion patterns.
//===----------------------------------------------------------------------===//

// Pass to lower from rng_uniform to stateless uniform pseudo RNG with LCG
// algorithm
struct RngUniformConversion : public OpConversionPattern<mhlo::RngUniformOp> {
  using OpConversionPattern<mhlo::RngUniformOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::RngUniformOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    // TODO(raikonenfnu): Handle other element types as well.
    auto minTy = adaptor.getOperands()[0].getType().dyn_cast<ShapedType>();
    auto maxTy = adaptor.getOperands()[0].getType().dyn_cast<ShapedType>();
    if (!minTy.getElementType().dyn_cast<FloatType>() ||
        !maxTy.getElementType().dyn_cast<FloatType>()) {
      return rewriter.notifyMatchFailure(
          op, "expected min/max for rng op to be FloatType");
    }
    auto target_ty = this->typeConverter->convertType(op.getResult().getType())
                         .cast<ShapedType>();
    if (!target_ty) {
      return rewriter.notifyMatchFailure(
          op, "expected target shape of rng op to be ShapedType");
    }
    auto loc = op.getLoc();
    Value initTensor =
        getInitTensorFor(rewriter, loc, target_ty, op, adaptor.getOperands());
    // Creates index map using target matrix's rank.
    auto targetRank = target_ty.getRank();
    SmallVector<AffineMap, 3> indexingMaps(
        2, AffineMap::get(targetRank, /*symbolCount=*/0,
                          SmallVector<AffineExpr>({}), rewriter.getContext()));
    indexingMaps.push_back(rewriter.getMultiDimIdentityMap(targetRank));
    const int kInitialSeed = 0;
    // Generic region with LCG Algorithm that make use of element index from:
    // https://reviews.llvm.org/D101364
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, /*resultTensors=*/target_ty,
        /*inputs=*/
        ValueRange{adaptor.getOperands()[0], adaptor.getOperands()[1]},
        /*outputs=*/initTensor, indexingMaps,
        getParallelAndReductionIterators(/*nLoops=*/targetRank,
                                         /*nReduction=*/0),
        [&](OpBuilder& b, Location loc, ValueRange args) {
          llvm::SmallVector<Value> updateVec = {b.create<arith::ConstantOp>(
              loc, b.getI32IntegerAttr(kInitialSeed))};
          Value multiplier =
              b.create<arith::ConstantOp>(loc, b.getI32IntegerAttr(1103515245));
          Value incrementStep =
              b.create<arith::ConstantOp>(loc, b.getI32IntegerAttr(12345));
          // For output matrix with rank N:
          // temp1 = (cast(I32, index(D.0)) + seed) * mult + incr
          // ...
          // tempN = (cast(I32, index(D.(N))) + tempN_1) * mult + incr
          for (int i = 0; i < targetRank; i++) {
            Value update = updateVec.back();
            Value ind = b.create<linalg::IndexOp>(loc, i);
            Value castInd =
                b.create<arith::IndexCastOp>(loc, b.getI32Type(), ind);
            Value addRes = b.create<arith::AddIOp>(loc, castInd, update);
            Value multRes = b.create<arith::MulIOp>(loc, addRes, multiplier);
            Value incRes = b.create<arith::AddIOp>(loc, multRes, incrementStep);
            updateVec.push_back(incRes);
          }
          // Scaling = (max - min) * const(F64, 2.3283064E-10)
          // which is derived from rand(min,max) = rand()/(RAND_MAX/(max-min)).
          Value epsilon = b.create<arith::ConstantOp>(
              loc, b.getFloatAttr(args[0].getType(), 2.3283064E-10));
          Value range = b.create<arith::SubFOp>(loc, args[1], args[0]);
          Value scale = b.create<arith::MulFOp>(loc, range, epsilon);
          // Res = cast(T, cast(F64, tempN) * scaling + min)
          Value updateCast = b.create<arith::UIToFPOp>(
              loc, target_ty.getElementType(), updateVec.back());
          Value scaleUpdate = b.create<arith::MulFOp>(loc, updateCast, scale);
          Value res = b.create<arith::AddFOp>(loc, scaleUpdate, args[0]);
          b.create<linalg::YieldOp>(loc, res);
        },
        pruneAttributeList(op));
    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }
};

//===----------------------------------------------------------------------===//
// mhlo.Einsum conversion patterns.
//===----------------------------------------------------------------------===//

// Looks through a set of dimension that has been marked as reduction axes,
// if it is found within the set, then we set it as "reduction", otherwise
// we can label it as "parallel".
SmallVector<StringRef, 3> getEinsumLoopsAttrs(
    const llvm::SmallSetVector<StringRef, 4>& inputInd,
    const llvm::SmallSetVector<StringRef, 4>& reductionDims) {
  SmallVector<StringRef, 3> res;
  for (StringRef dim : inputInd) {
    if (!reductionDims.contains(dim)) {
      res.push_back(getParallelIteratorTypeName());
    } else {
      res.push_back(getReductionIteratorTypeName());
    }
  }
  return res;
}

SmallVector<Value, 2> extractDynamicEinsumSizes(
    OpBuilder& b, Location loc, Value lhs, Value rhs,
    const SmallVector<std::string>& lhsLoopVec,
    const SmallVector<std::string>& rhsLoopVec,
    const SmallVector<std::string>& outputLoopVec) {
  SmallVector<Value, 2> dynSizes;
  for (const std::string& dimInd : outputLoopVec) {
    Value dimSize;
    const auto* dimIndIt =
        std::find(lhsLoopVec.begin(), lhsLoopVec.end(), dimInd);
    if (dimIndIt != lhsLoopVec.end()) {
      // Query from lhs vars.
      auto dimIndPos = dimIndIt - lhsLoopVec.begin();
      auto lhsShape = lhs.getType().dyn_cast<RankedTensorType>().getShape();
      if (lhsShape[dimIndPos] != ShapedType::kDynamicSize) continue;
      dimSize = b.create<tensor::DimOp>(loc, lhs, dimIndPos);
    } else {
      // query from rhs vars.
      dimIndIt = std::find(rhsLoopVec.begin(), rhsLoopVec.end(), dimInd);
      auto dimIndPos = dimIndIt - rhsLoopVec.begin();
      auto rhsShape = rhs.getType().dyn_cast<RankedTensorType>().getShape();
      if (rhsShape[dimIndPos] != ShapedType::kDynamicSize) continue;
      dimSize = b.create<tensor::DimOp>(loc, rhs, dimIndPos);
    }
    dynSizes.push_back(dimSize);
  }
  return dynSizes;
}

// Adds indices/axes that are missing from output set.
llvm::SmallSetVector<StringRef, 4> findSummationAxes(
    const llvm::SmallSetVector<StringRef, 4>& inputSet,
    const llvm::SmallSetVector<StringRef, 4>& outputSet) {
  llvm::SmallSetVector<StringRef, 4> summationAxes;
  for (StringRef ind : inputSet) {
    if (!outputSet.contains(ind)) summationAxes.insert(ind);
  }
  return summationAxes;
}

// Given a 1:1 map from std::string -> affine dimension expression
// we can get the affine expression of dimensions that an
// operand will access based on the input_str of einsum_config.
// For example:
// let string_dim_umap = {'a' : d0, 'b' : d1, 'c' : d2}
// for einsum_config "abc,cb->acb"
// first_input_operand will get umap[{"a","b","c"}] -> (d0, d1, d2).
// second_input_operand will get umap[{"c","b"}] -> (d2, d1).
// output_operand will get umap[{"a","c","b"}] -> (d0, d2, d1).
SmallVector<AffineExpr> getExprFromConfig(
    const SmallVector<std::string>& loopDims,
    const DenseMap<StringRef, AffineExpr>& strAffineDimUmap) {
  SmallVector<AffineExpr> exprs;
  for (const auto& dim : loopDims) {
    exprs.push_back(strAffineDimUmap.lookup(dim));
  }
  return exprs;
}

// Convert mhlo.einsum op into linalg.generic.
// Algorithm in general 3 steps:

// Step1) Dissect entire einsum_config to different operands
// e.g f("abc,cd->abd") = {lhs:["abc"], rhs:["cd"], out:["abd"]}.

// Step2) Split up the string into vector of the elements
// e.g {lhs:["abc"], rhs:["cd"], out:["abd"]} = {lhs:["a","b","c"],
// rhs:["c","d"], out:["a","b","d"]}.

// Step3) Convert the vector into data access
// patern represented by affineMaps with affineDimensions e.g
// {lhs:["a","b","c"], rhs:["c","d"], out:["a","b","d"]} = {lhs:[d0,d1,d2],
// rhs:[d2,d3], out:[d0,d1,d3]}.
class EinsumToLinalgConverter : public OpConversionPattern<mhlo::EinsumOp> {
 public:
  using OpConversionPattern<mhlo::EinsumOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::EinsumOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto getRank = [](Value v) {
      return v.getType().cast<ShapedType>().getRank();
    };
    auto einsumConfig = op.einsum_config();

    // With the assumption of binary input operand and single output
    // get the inputs and output operands' indices.
    // einsum_config = "lhs_loop,rhs_loop->out_loop"
    std::size_t posArrow = einsumConfig.find(kArrow);
    std::size_t posComma = einsumConfig.find(kComma);

    StringRef lhsLoop = einsumConfig.substr(0, posComma);
    StringRef rhsLoop = einsumConfig.substr(
        posComma + kComma.size(), posArrow - (posComma + kComma.size()));
    StringRef outLoop = einsumConfig.substr(posArrow + kArrow.size());

    // Check for Invalid Configs.
    // 1.Check that there is only maximum 2 inputs
    // 2.Check that there is only maximum 1 output
    // 3.Check that there is 1 kArrow
    if (rhsLoop.find(kComma) != std::string::npos ||
        outLoop.find(kComma) != std::string::npos ||
        outLoop.find(kArrow) != std::string::npos) {
      return rewriter.notifyMatchFailure(op, "Invalid einsum config!");
    }

    // Find result type, if on tensors.
    auto resultTy = this->typeConverter->convertType(getHloOpResultType(op))
                        .dyn_cast<RankedTensorType>();

    // Check result type compatibility.
    if (!resultTy || !(resultTy.getElementType().isSignlessIntOrFloat())) {
      return rewriter.notifyMatchFailure(op, "Invalid result type");
    }

    // Convert the representation to vector<string>.
    SmallVector<std::string> lhsEin =
        getEinsumConfigAsVector(lhsLoop, getRank(adaptor.lhs()));
    SmallVector<std::string> rhsEin =
        getEinsumConfigAsVector(rhsLoop, getRank(adaptor.rhs()));
    SmallVector<std::string> outEin =
        getEinsumConfigAsVector(outLoop, resultTy.getRank());

    if (!checkBatchHasEqualRank(lhsEin.size(), lhsLoop, rhsEin.size(), rhsLoop,
                                outEin.size(), outLoop)) {
      return rewriter.notifyMatchFailure(
          op, "Invalid elipsis('...') within einsum config!");
    }

    // Find all unique indices in the input and output.
    llvm::SmallSetVector<StringRef, 4> inputInd;
    llvm::SmallSetVector<StringRef, 4> outputInd;

    inputInd.insert(lhsEin.begin(), lhsEin.end());
    inputInd.insert(rhsEin.begin(), rhsEin.end());
    outputInd.insert(outEin.begin(), outEin.end());

    llvm::SmallSetVector<StringRef, 4> reductionAxe =
        findSummationAxes(inputInd, outputInd);

    // Find input/output values and types.
    auto loc = op.getLoc();

    // Prepare init tensor for linalg.generic op.
    auto dynSizes = extractDynamicEinsumSizes(
        rewriter, loc, adaptor.lhs(), adaptor.rhs(), lhsEin, rhsEin, outEin);
    Value output = getInitTensor(rewriter, loc, resultTy, dynSizes);
    if (!reductionAxe.empty()) {
      output = fillTensorWithZeros(rewriter, loc, output);
    }

    // Create indexing maps.
    // Create a 1:1 map from f:strDimension -> affineDimension.
    int64_t nloops = inputInd.size();
    DenseMap<StringRef, AffineExpr> strAffineDimUmap;
    for (auto& it : llvm::enumerate(inputInd)) {
      strAffineDimUmap[it.value()] = rewriter.getAffineDimExpr(it.index());
    }

    // From einsum_config of each operand in vector<string>, generate
    // the equivalent vector<AffineExpr>.
    SmallVector<AffineMap, 4> maps;
    for (const SmallVector<std::string>& loopOperand :
         {lhsEin, rhsEin, outEin}) {
      auto exprs = getExprFromConfig(loopOperand, strAffineDimUmap);
      maps.push_back(AffineMap::get(nloops, 0, exprs, rewriter.getContext()));
    }

    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, resultTy ? resultTy : TypeRange{}, adaptor.getOperands(), output,
        maps, getEinsumLoopsAttrs(inputInd, reductionAxe),
        [reductionAxe](OpBuilder& b, Location nestedLoc, ValueRange args) {
          Value resultVal =
              b.create<mlir::arith::MulFOp>(nestedLoc, args[0], args[1]);
          if (!reductionAxe.empty()) {
            resultVal =
                b.create<mlir::arith::AddFOp>(nestedLoc, args[2], resultVal);
          }
          b.create<linalg::YieldOp>(nestedLoc, resultVal);
        },
        pruneAttributeList(op));
    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }

 private:
  static constexpr StringRef kArrow = "->";
  static constexpr StringRef kComma = ",";
  static constexpr StringRef kEllipsis = "...";

  static bool checkBatchHasEqualRank(size_t lhsRank, StringRef lhsLoop,
                                     size_t rhsRank, StringRef rhsLoop,
                                     size_t outRank, StringRef outLoop);
  static SmallVector<std::string> getEinsumConfigAsVector(StringRef loop,
                                                          size_t operandRank);
};

// Definition of util const member variables.
constexpr StringRef EinsumToLinalgConverter::kArrow;
constexpr StringRef EinsumToLinalgConverter::kComma;
constexpr StringRef EinsumToLinalgConverter::kEllipsis;

// Convert the representation from string/vector<char> to vector<string>.
// i.e ("abc") -> {"a", "b", "c"}. For cases with ellipsis with batch rank 3:
// get loop_dim = f("ab...cde") = {"a","b","0","1","2","c","d","e"}
SmallVector<std::string> EinsumToLinalgConverter::getEinsumConfigAsVector(
    StringRef loop, size_t operandRank) {
  SmallVector<std::string> loopDim;
  size_t preElip = loop.find(kEllipsis);
  bool hasElip = preElip != std::string::npos;
  if (!hasElip) preElip = loop.size();
  // Add the dimension until the end or up to ellipsis if it exist.
  for (int preElipInd = 0; preElipInd < preElip; preElipInd++) {
    loopDim.push_back(loop.substr(preElipInd, 1).str());
  }
  if (!hasElip) return loopDim;
  // Case where Ellipsis presence:
  size_t nonBatchRank = loop.size() - kEllipsis.size();
  size_t batchRank = operandRank - nonBatchRank;
  // Add the batch dimension ("0",...,"N") where N is rank of batch into the
  // loop.
  for (int batchInd = 0; batchInd < batchRank; batchInd++) {
    loopDim.push_back(std::to_string(batchInd));
  }
  // Add the dimension after ellipsis into the loop.
  int postElip = preElip + kEllipsis.size();
  for (int postElipInd = postElip; postElipInd < loop.size(); postElipInd++) {
    loopDim.push_back(loop.substr(postElipInd, 1).str());
  }
  return loopDim;
}

// Returns true if all operand's batch has same rank.
bool EinsumToLinalgConverter::checkBatchHasEqualRank(
    size_t lhsRank, StringRef lhsLoop, size_t rhsRank, StringRef rhsLoop,
    size_t outRank, StringRef outLoop) {
  SmallVector<int, 3> batchRankVec;
  if (lhsRank != lhsLoop.size()) {
    size_t lhsBatchRank = lhsRank - (lhsLoop.size() - kEllipsis.size());
    batchRankVec.push_back(lhsBatchRank);
  }
  if (rhsRank != rhsLoop.size()) {
    size_t rhsBatchRank = rhsRank - (rhsLoop.size() - kEllipsis.size());
    batchRankVec.push_back(rhsBatchRank);
  }
  if (outRank != outLoop.size()) {
    size_t outBatchRank = outRank - (outLoop.size() - kEllipsis.size());
    batchRankVec.push_back(outBatchRank);
  }
  bool batchHasEqualRank = true;

  // Condition is valid if only 1 operand or less have batches.
  if (batchRankVec.size() < 2) return batchHasEqualRank;
  if (!std::equal(batchRankVec.begin() + 1, batchRankVec.end(),
                  batchRankVec.begin()) &&
      batchRankVec.size() > 1)
    batchHasEqualRank = false;
  return batchHasEqualRank;
}

template <typename OpTy>
class PointwiseToLinalgConverter : public OpConversionPattern<OpTy> {
 public:
  using OpConversionPattern<OpTy>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      OpTy op, typename OpTy::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    // Find maximum rank / number of loops.
    auto getRank = [](Value v) {
      return v.getType().cast<ShapedType>().getRank();
    };
    auto isScalar = [&](Value v) { return getRank(v) == 0; };
    auto it = llvm::find_if_not(adaptor.getOperands(), isScalar);
    Value maxRankArg =
        it != adaptor.getOperands().end() ? *it : adaptor.getOperands().front();
    int64_t nloops = getRank(maxRankArg);

    // Apply only if all operands are scalar or have the same rank. Some ops,
    // like `mhlo.select`, support implicit broadcasting of scalars.
    if (!llvm::all_of(adaptor.getOperands(), [&](Value v) {
          int64_t r = getRank(v);
          return r == 0 || r == nloops;
        })) {
      return rewriter.notifyMatchFailure(
          op, "Operands must be os same rank or scalar.");
    }

    // Find result type, if on tensors.
    Optional<ShapedType> resultTy;
    resultTy = this->typeConverter->convertType(op->getResultTypes().front())
                   .template dyn_cast<ShapedType>();

    // Check result type compatibility.
    if (!resultTy || !resultTy->hasRank() || resultTy->getRank() != nloops ||
        !(resultTy->getElementType().isSignlessIntOrFloat() ||
          resultTy->getElementType().isa<ComplexType>())) {
      return rewriter.notifyMatchFailure(
          op, "mismatched operand/result types or iterator count");
    }

    // Find input/output values and types.
    auto loc = op.getLoc();
    ValueRange inputs = adaptor.getOperands();
    Value output =
        getInitTensorFor(rewriter, loc, *resultTy, op, adaptor.getOperands());

    // Create indexing maps.
    AffineMap scalarMap = AffineMap::get(nloops, 0, rewriter.getContext());
    AffineMap idMap = rewriter.getMultiDimIdentityMap(nloops);
    SmallVector<AffineMap, 4> maps;
    for (Value v : inputs) maps.push_back(isScalar(v) ? scalarMap : idMap);
    maps.push_back(idMap);

    // Build `linalg.generic` op.
    bool failed = false;
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, resultTy ? *resultTy : TypeRange{}, inputs, output, maps,
        getNParallelLoopsAttrs(nloops),
        [&](OpBuilder& nestedBuilder, Location /*nested_loc*/,
            ValueRange args) {
          Type innerResultTy = getElementTypeOrSelf(output);
          auto argvec = llvm::to_vector<2>(args.take_front(inputs.size()));
          auto semiring = preSparsify(op, argvec, innerResultTy, &rewriter);
          Value innerResult = mhlo::MhloOpToStdScalarOp::map<OpTy>(
              op, innerResultTy, argvec, &rewriter);
          if (innerResult == nullptr) {
            failed = true;
          } else {
            innerResult = postSparsify(op, semiring, innerResult, &rewriter);
            nestedBuilder.create<linalg::YieldOp>(loc, innerResult);
          }
        },
        pruneAttributeList(op));
    if (failed) return failure();

    rewriter.replaceOp(op, linalgOp->getResults());
    return success();
  }
};

template <typename MhloOp>
class ScalarPointwiseToStandardConverter : public OpConversionPattern<MhloOp> {
 public:
  using OpConversionPattern<MhloOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      MhloOp mhloOp, ConversionPatternRewriter& rewriter) const final {
    auto loc = mhloOp.getLoc();
    auto argType =
        mhloOp.getOperand(0).getType().template dyn_cast<ShapedType>();
    if (!argType || !argType.getElementType().isSignlessIntOrFloat() ||
        (argType.getRank() != 0)) {
      return failure();
    }

    // Create two loads from the input.
    auto lhs = rewriter.create<memref::LoadOp>(loc, mhloOp.lhs());
    auto rhs = rewriter.create<memref::LoadOp>(loc, mhloOp.rhs());
    Value opResult = mhlo::MhloOpToStdScalarOp::map<MhloOp>(
        mhloOp, argType.getElementType(), llvm::ArrayRef<Value>{lhs, rhs},
        &rewriter);
    rewriter.create<memref::StoreOp>(loc, opResult, mhloOp.out());
    rewriter.eraseOp(mhloOp);
    return success();
  }
};

/// Base class for lowering HLO operations that have one operand and one result,
/// and are semantically equivalent to a copy of the input to the output (like
/// transpose, some reshape, etc.). The derived classes need to provide a method
/// `getIndexingMaps` that returns AffineMaps for the index maps of the input
/// and the output.
template <typename Derived, typename OpTy>
class DataMovementOpConverter : public OpConversionPattern<OpTy> {
 public:
  using OpConversionPattern<OpTy>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      OpTy op, typename OpTy::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!verifyHloOpBufferOrTensorSemantics(op)) return failure();
    auto resultType = getHloOpResultType(op);
    resultType = this->typeConverter->convertType(resultType)
                     .template cast<ShapedType>();

    SmallVector<AffineMap, 2> indexingMaps =
        Derived::getIndexingMaps(op, &rewriter);
    if (indexingMaps.empty()) return failure();

    auto nloops = resultType.getRank();
    auto loc = op.getLoc();
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc,
        /*resultTensorTypes=*/resultType,
        /*inputs=*/adaptor.getOperands().front(),
        /*outputBuffers=*/

        ValueRange{getInitTensorFor(rewriter, loc, resultType, op,
                                    adaptor.getOperands())},
        indexingMaps, getNParallelLoopsAttrs(nloops),
        [&](OpBuilder& nestedBuilder, Location /*nested_loc*/,
            ValueRange args) {
          nestedBuilder.create<linalg::YieldOp>(loc, *args.begin());
        },
        pruneAttributeList(op));
    rewriter.replaceOp(op, linalgOp.getOperation()->getResults());
    return success();
  }
};

/// Pattern to convert BroadcastOp to Linalg ops.
template <typename OpTy>
class BroadcastConverter
    : public DataMovementOpConverter<BroadcastConverter<OpTy>, OpTy> {
 public:
  using DataMovementOpConverter<BroadcastConverter,
                                OpTy>::DataMovementOpConverter;

  static SmallVector<AffineMap, 2> getIndexingMaps(OpTy broadcastOp,
                                                   Builder* b) {
    ShapedType inputType =
        broadcastOp.operand().getType().template cast<ShapedType>();
    unsigned inputRank = inputType.getRank();
    unsigned nloops = getHloOpResultType(broadcastOp).getRank();

    // BroadcastOp prepends the dimensions in the `broadcast_sizes` attribute to
    // the input's dimensions.
    unsigned numPrependedDims = llvm::size(broadcastOp.broadcast_sizes());
    SmallVector<AffineExpr, 4> inputDimExprs;
    inputDimExprs.reserve(inputRank);
    for (unsigned i = 0; i < inputRank; ++i) {
      inputDimExprs.push_back(b->getAffineDimExpr(numPrependedDims + i));
    }

    AffineMap inputMap;
    MLIRContext* context = b->getContext();
    if (inputDimExprs.empty()) {
      // The input is a scalar, i.e. this is a scalar broadcast op.
      inputMap = AffineMap::get(nloops, /*symbolCount=*/0, context);
    } else {
      inputMap =
          AffineMap::get(nloops, /*symbolCount=*/0, inputDimExprs, context);
    }
    return {inputMap, b->getMultiDimIdentityMap(nloops)};
  }
};

class HloBroadcastInDimConverter
    : public DataMovementOpConverter<HloBroadcastInDimConverter,
                                     mhlo::BroadcastInDimOp> {
 public:
  using DataMovementOpConverter<
      HloBroadcastInDimConverter,
      mhlo::BroadcastInDimOp>::DataMovementOpConverter;

  static SmallVector<AffineMap, 2> getIndexingMaps(
      mhlo::BroadcastInDimOp broadcastOp, Builder* b) {
    auto resultType = getHloOpResultType(broadcastOp);
    auto operandType =
        broadcastOp.operand().getType().template cast<ShapedType>();
    unsigned nloops = resultType.getRank();

    // The input is a scalar, i.e. this is a scalar broadcast op.
    if (operandType.getRank() == 0) {
      return {AffineMap::get(nloops, /*symbolCount=*/0, b->getContext()),
              b->getMultiDimIdentityMap(nloops)};
    }

    auto operandShape = operandType.getShape();
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(nloops);

    if (broadcastOp.broadcast_dimensions()) {
      for (const auto& broadcastDim :
           enumerate(broadcastOp.broadcast_dimensions().getValues<APInt>())) {
        int size = broadcastDim.value().getSExtValue();
        bool expansionNeeded = operandShape[broadcastDim.index()] == 1 &&
                               resultType.getShape()[size] != 1;
        dimExprs.push_back(expansionNeeded ? b->getAffineConstantExpr(0)
                                           : b->getAffineDimExpr(size));
      }
    }
    return {
        AffineMap::get(nloops, /*symbolCount=*/0, dimExprs, b->getContext()),
        b->getMultiDimIdentityMap(nloops)};
  }
};

// If the input has a static shape we know exactly when the broadcast must
// expand (the dimension is 1, which also trivially expands to 1) or will never
// expand (the dimension is not 1). We can also source the information from the
// optionally provided attrbibutes on statically known broadcasting behavior.
// This means we can lower the broadcast just as we would lower a fully static
// broadcast and go directly to `linalg.generic`.

// This also covers the important case of broadcasting a scalar. Ideally the
// pattern (`mhlo.constant` -> `mhlo.dynamic_broadcast_in_dim`) should be
// converted to a tensor dialect op similar to TF's `ConstantLikeOp`.
class HloDynamicBroadcastInDimConverter
    : public OpConversionPattern<mhlo::DynamicBroadcastInDimOp> {
 public:
  using OpConversionPattern<mhlo::DynamicBroadcastInDimOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::DynamicBroadcastInDimOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    Value operand = adaptor.operand();
    auto operandType = operand.getType().dyn_cast<RankedTensorType>();
    if (!operandType) return failure();
    auto resultType =
        typeConverter->convertType(op.getType()).dyn_cast<RankedTensorType>();
    if (!resultType) return failure();

    // Determine dimension expressions based on whether the dimension is
    // expanding (0) or non-expanding (identity), and fail if we cannot decide
    // this.
    SmallVector<AffineExpr> dimExprs(operandType.getRank(), nullptr);

    // Use static type info.
    auto bcastDims = llvm::to_vector(
        llvm::map_range(op.broadcast_dimensions(), [](const APInt& d) {
          return static_cast<int64_t>(d.getLimitedValue());
        }));
    for (const auto& it : llvm::enumerate(operandType.getShape())) {
      if (ShapedType::isDynamic(it.value())) continue;
      bool isExpanding = it.value() == 1;
      dimExprs[it.index()] =
          isExpanding ? rewriter.getAffineConstantExpr(0)
                      : rewriter.getAffineDimExpr(bcastDims[it.index()]);
    }

    // Use annotated expansion behavior, if available.
    if (op.known_expanding_dimensions()) {
      for (const auto& it :
           op.known_expanding_dimensions()->getValues<APInt>()) {
        auto i = it.getLimitedValue();
        dimExprs[i] = rewriter.getAffineConstantExpr(0);
      }
    }
    if (op.known_nonexpanding_dimensions()) {
      for (const auto& it :
           op.known_nonexpanding_dimensions()->getValues<APInt>()) {
        auto i = it.getLimitedValue();
        dimExprs[i] = rewriter.getAffineDimExpr(bcastDims[i]);
      }
    }

    // Fail if unknown expansion behavior remains.
    if (!llvm::all_of(dimExprs, [](AffineExpr expr) { return expr; }))
      return failure();

    // Materialize `linalg.generic` op.
    Location loc = op.getLoc();
    int64_t nloops = resultType.getRank();
    Value init =
        getInitTensorFor(rewriter, loc, resultType, op, adaptor.getOperands());
    rewriter.replaceOpWithNewOp<linalg::GenericOp>(
        op, TypeRange{init.getType()}, ValueRange{operand},
        /*outputBuffers=*/ValueRange{init},
        llvm::makeArrayRef(
            {AffineMap::get(/*dimCount=*/nloops, /*symbolCount=*/0, dimExprs,
                            rewriter.getContext()),
             rewriter.getMultiDimIdentityMap(nloops)}),
        getNParallelLoopsAttrs(nloops),
        [&](OpBuilder& nestedBuilder, Location /*nested_loc*/,
            ValueRange args) {
          nestedBuilder.create<linalg::YieldOp>(loc, *args.begin());
        },
        pruneAttributeList(op));
    return success();
  }
};

template <typename OpTy>
class TransposeConverter
    : public DataMovementOpConverter<TransposeConverter<OpTy>, OpTy> {
 public:
  using DataMovementOpConverter<TransposeConverter<OpTy>,
                                OpTy>::DataMovementOpConverter;
  static SmallVector<AffineMap, 2> getIndexingMaps(OpTy op, Builder* b) {
    auto resultType = getHloOpResultType(op).template cast<ShapedType>();
    auto nloops = resultType.getRank();
    SmallVector<AffineExpr, 2> inputExprs;
    inputExprs.resize(resultType.getRank());
    for (const auto& permutation : llvm::enumerate(op.permutation())) {
      inputExprs[permutation.value().getZExtValue()] =
          b->getAffineDimExpr(permutation.index());
    }
    return {
        AffineMap::get(nloops, /*symbolCount=*/0, inputExprs, b->getContext()),
        b->getMultiDimIdentityMap(nloops)};
  }
};

// Lowers mhlo.RealDynamicSliceOp to tensor.extract_slice and other
// arith/tensor dialect ops.
class RealDynamicSliceConverter
    : public OpConversionPattern<mhlo::RealDynamicSliceOp> {
 public:
  using OpConversionPattern<mhlo::RealDynamicSliceOp>::OpConversionPattern;

  // Computes size of a slice as
  //   size = ceil((limit - start)/stride)
  static Value computeSize(Location loc, Value start, Value limit, Value stride,
                           ConversionPatternRewriter& b) {
    Value delta = b.create<arith::SubIOp>(loc, limit, start);
    Value ret = b.create<arith::CeilDivUIOp>(loc, delta, stride);
    if (ret.getType().isIndex()) return ret;
    return b.create<arith::IndexCastOp>(loc, b.getIndexType(), ret);
  }

  LogicalResult matchAndRewrite(
      mhlo::RealDynamicSliceOp realDynamicSliceOp, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    Location loc = realDynamicSliceOp.getLoc();
    auto argType = adaptor.operand().getType().dyn_cast<ShapedType>();
    if (!argType || !argType.hasRank()) {
      return rewriter.notifyMatchFailure(realDynamicSliceOp,
                                         "require known-rank args");
    }

    Type dimElementType = getElementTypeOrSelf(adaptor.start_indices());
    if (getElementTypeOrSelf(adaptor.limit_indices()) != dimElementType ||
        getElementTypeOrSelf(adaptor.strides()) != dimElementType) {
      return rewriter.notifyMatchFailure(
          realDynamicSliceOp,
          "requires same element type for all dimension specification");
    }
    Type arithType =
        dimElementType.isIndex() ? rewriter.getI64Type() : dimElementType;
    Type indexType = rewriter.getIndexType();

    auto resultType =
        this->typeConverter->convertType(realDynamicSliceOp.getType())
            .cast<RankedTensorType>();
    Value zero =
        rewriter.create<arith::ConstantOp>(loc, IntegerAttr::get(arithType, 0));
    SmallVector<OpFoldResult, 4> offsets, sizes, strides;
    SmallVector<Type, 3> clampType(3, arithType);
    for (auto i : llvm::seq<unsigned>(0, argType.getRank())) {
      Value dim = rewriter.create<arith::ConstantIndexOp>(loc, i);
      Value start =
          rewriter.create<tensor::ExtractOp>(loc, adaptor.start_indices(), dim);
      Value limit =
          rewriter.create<tensor::ExtractOp>(loc, adaptor.limit_indices(), dim);
      Value stride =
          rewriter.create<tensor::ExtractOp>(loc, adaptor.strides(), dim);

      // Compute i-th dimension size of the result : size[i].
      // If the i-th dimension of the result type is known, we go ahead with it
      // else we compute it using limit, start and stride values.
      int64_t resultDimSize = resultType.getDimSize(i);
      Value size =
          ShapedType::isDynamic(resultDimSize)
              ? computeSize(loc, start, limit, stride, rewriter)
              : rewriter.create<arith::ConstantIndexOp>(loc, resultDimSize);

      // Fetch i-th dimension size of the operand and calculate upper bound as
      //   ub = operand_dim[i] - size[i]
      Value operandDimSize =
          rewriter.createOrFold<tensor::DimOp>(loc, adaptor.operand(), dim);
      Value upperBound =
          rewriter.createOrFold<arith::SubIOp>(loc, operandDimSize, size);

      // We clamp the start_index to keep it bounded as
      //   0 <= start_index[i] <= ub
      // Clamp does not support index type, so cast to integer type.
      start = rewriter.createOrFold<arith::IndexCastOp>(loc, arithType, start);
      upperBound =
          rewriter.createOrFold<arith::IndexCastOp>(loc, arithType, upperBound);
      start = mhlo::MhloOpToStdScalarOp::map<mhlo::ClampOp>(
          loc, arithType, clampType, ValueRange{zero, start, upperBound},
          &rewriter);

      offsets.push_back(
          rewriter.createOrFold<arith::IndexCastOp>(loc, indexType, start));
      if (ShapedType::isDynamic(resultDimSize))
        sizes.push_back(size);
      else
        sizes.push_back(IntegerAttr::get(indexType, resultDimSize));
      strides.push_back(
          rewriter.createOrFold<arith::IndexCastOp>(loc, indexType, stride));
    }

    rewriter.replaceOpWithNewOp<tensor::ExtractSliceOp>(
        realDynamicSliceOp, resultType, adaptor.operand(), offsets, sizes,
        strides);
    return success();
  }
};

// Converts reshape ops that can be proven to be either a collapse of dimensions
// or expansion of dimensions of the operand.
class ReshapeOpConverter : public OpConversionPattern<mhlo::ReshapeOp> {
 public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ReshapeOp reshapeOp, mhlo::ReshapeOp::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!verifyHloOpBufferOrTensorSemantics(reshapeOp)) return failure();
    auto operand = adaptor.operand();
    auto operandType = operand.getType().cast<ShapedType>();
    auto elemType = operandType.getElementType();
    auto resultType = reshapeOp.getType().cast<ShapedType>();

    if (!resultType.hasStaticShape()) return failure();

    resultType = typeConverter->convertType(resultType).cast<ShapedType>();

    // Special case where the result is a scalar.
    if (resultType.getRank() == 0 && !operandType.hasStaticShape()) {
      // This means all dimensions of the operand need to be 1. We add a cast to
      // cast the dynamic dimensions to 1.
      auto staticType = RankedTensorType::get(
          llvm::SmallVector<int64_t>(operandType.getRank(), 1), elemType);
      operand = rewriter.create<tensor::CastOp>(reshapeOp.getLoc(), staticType,
                                                operand);
      rewriter.replaceOpWithNewOp<tensor::CollapseShapeOp>(
          reshapeOp, resultType, operand, ArrayRef<ReassociationIndices>{});
      return success();
    }

    // Compute the reassociation maps for the linalg operation. This will
    // succeed if the reshape can be done with a single expand_shape or
    // collapse_shape.
    if (Optional<SmallVector<ReassociationIndices>> reassociationMap =
            getReassociationIndicesForReshape(operandType, resultType)) {
      if (resultType.getRank() < operandType.getRank()) {
        // We have found a working reassociation map. If the operand is dynamic,
        // we first need to cast all unknown dimensions in the input that get
        // collapsed to a static-sized dimension in the output, to 1.
        SmallVector<int64_t> shape(operandType.getShape().begin(),
                                   operandType.getShape().end());
        for (const auto& map : llvm::enumerate(*reassociationMap)) {
          // If the result dim is dynamic, we do not mind dynamic entries in the
          // source.
          if (resultType.isDynamicDim(map.index())) continue;
          for (auto targetDim : map.value()) {
            if (shape[targetDim] == ShapedType::kDynamicSize)
              shape[targetDim] = 1;
          }
        }
        auto newOperandType = RankedTensorType::get(shape, elemType);
        if (newOperandType != operandType) {
          operand = rewriter.create<tensor::CastOp>(reshapeOp.getLoc(),
                                                    newOperandType, operand);
        }
        rewriter.replaceOpWithNewOp<tensor::CollapseShapeOp>(
            reshapeOp, resultType, operand, *reassociationMap);
      } else {
        rewriter.replaceOpWithNewOp<tensor::ExpandShapeOp>(
            reshapeOp, resultType, operand, *reassociationMap);
      }
      return success();
    }

    Value collapsedOp = operand;
    Location loc = reshapeOp.getLoc();
    auto getIdentityExprs = [&rewriter](int64_t n) {
      SmallVector<AffineExpr, 4> exprs;
      for (int i = 0; i < n; ++i) exprs.push_back(rewriter.getAffineDimExpr(i));
      return exprs;
    };
    // Otherwise, we need to first reduce all source dimensions into one and
    // then expand to the destination dimensions. If there is only a single
    // source dimension, the reduce step can be skipped. TensorCollapseShape
    // expects a different rank of operand and result.
    if (operandType.getRank() != 1) {
      SmallVector<ReassociationExprs, 4> collapsingMap = {
          // Use operand_type here because we need to collapse all operands
          // dimensions.
          getIdentityExprs(operandType.getRank())};

      collapsedOp =
          rewriter.create<tensor::CollapseShapeOp>(loc, operand, collapsingMap);
    }
    // Cast to a known static type if the input has dynamic dimensions.
    int64_t totalElems = resultType.getNumElements();
    auto collapsedType = RankedTensorType::get({totalElems}, elemType);
    collapsedOp =
        rewriter.create<tensor::CastOp>(loc, collapsedType, collapsedOp);
    if (resultType.getRank() == 1) {
      rewriter.replaceOp(reshapeOp, collapsedOp);
    } else {
      SmallVector<ReassociationExprs, 4> expandingMap = {
          // Use resultType here because we need to expand to all result
          // dimensions.
          getIdentityExprs(resultType.getRank())};
      rewriter.replaceOpWithNewOp<tensor::ExpandShapeOp>(
          reshapeOp, resultType, collapsedOp, expandingMap);
    }
    return success();
  }
};

template <typename OpTy>
class IotaConverter : public OpConversionPattern<OpTy> {
 public:
  using OpConversionPattern<OpTy>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      OpTy iota_op, typename OpTy::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    ShapedType resultShapedType = getHloOpResultType(iota_op);
    if (!resultShapedType) return failure();
    resultShapedType = this->typeConverter->convertType(resultShapedType)
                           .template dyn_cast<ShapedType>();

    Type resultElementType = resultShapedType.getElementType();

    // Construct the indexing maps needed for linalg.generic ops.
    unsigned nloops = resultShapedType.getRank();

    Location loc = iota_op.getLoc();
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc,
        /*resultTensorTypes=*/
        ArrayRef<Type>{resultShapedType},
        /*inputs=*/ValueRange{},
        /*outputBuffers=*/

        ValueRange{getInitTensorFor(rewriter, loc, resultShapedType, iota_op,
                                    adaptor.getOperands())},
        llvm::makeArrayRef(rewriter.getMultiDimIdentityMap(nloops)),
        getNParallelLoopsAttrs(nloops),
        [&](OpBuilder& nestedBuilder, Location nestedLoc, ValueRange /*args*/) {
          Value indexOp = nestedBuilder.create<linalg::IndexOp>(
              nestedLoc, iota_op.iota_dimension());
          Type unwrappedResultElementType = resultElementType;
          if (auto complexType =
                  unwrappedResultElementType.dyn_cast<ComplexType>())
            unwrappedResultElementType = complexType.getElementType();
          Value castOp = nestedBuilder.create<arith::IndexCastOp>(
              nestedLoc,
              nestedBuilder.getIntegerType(
                  unwrappedResultElementType.getIntOrFloatBitWidth()),
              indexOp);
          castOp = mhlo::MhloOpToStdScalarOp::map<mhlo::ConvertOp>(
              nestedLoc, resultElementType, castOp.getType(), castOp,
              &nestedBuilder);
          nestedBuilder.create<linalg::YieldOp>(nestedLoc, castOp);
        },
        pruneAttributeList(iota_op));
    rewriter.replaceOp(iota_op, linalgOp.result_tensors());
    return success();
  }
};

/// Converts mhlo.concatenate operation to a linalg.generic op.
struct ConcatenateConverter : public OpConversionPattern<mhlo::ConcatenateOp> {
  using OpConversionPattern<mhlo::ConcatenateOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ConcatenateOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    // Shortcut the one-operand case, simplifies code below.
    if (adaptor.getOperands().size() == 1) {
      rewriter.replaceOp(op, adaptor.getOperands()[0]);
      return success();
    }

    auto resultType = this->typeConverter->convertType(op.getResult().getType())
                          .dyn_cast<RankedTensorType>();
    if (!resultType) return failure();

    uint64_t dim = op.dimension();
    Location loc = op.getLoc();
    Value zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);

    // Allocate the output tensor with init_tensor.
    Value result =
        getInitTensorFor(rewriter, loc, resultType, op, adaptor.getOperands());

    // Generate a generic op to gather the elements of the concatenate. This is
    // awkward standalone but allows fusion with other generic ops.
    int64_t nloops = resultType.getRank();
    rewriter.replaceOpWithNewOp<linalg::GenericOp>(
        op,
        /*resultTensorTypes=*/resultType,
        /*inputs=*/ValueRange{}, /*outputBuffers=*/result,
        llvm::makeArrayRef(rewriter.getMultiDimIdentityMap(nloops)),
        getNParallelLoopsAttrs(nloops),
        [&](OpBuilder& nestedBuilder, Location loc, ValueRange) {
          OpBuilder b = nestedBuilder;
          Value concatDimSize = zero;
          Value result;

          SmallVector<Value, 4> extractIndices;
          extractIndices.reserve(nloops);
          for (int64_t i = 0; i < nloops; i++) {
            extractIndices.push_back(b.create<linalg::IndexOp>(loc, i));
          }

          Value indexOp = b.create<linalg::IndexOp>(loc, dim);
          for (auto& it : llvm::enumerate(adaptor.getOperands())) {
            Value arg = it.value();
            Value newConcatDimSize;
            scf::IfOp ifOp;
            if (it.index() != (adaptor.getOperands().size() - 1)) {
              // Calculate how far along we have iterated along the concatenate
              // dimension. That way we can tell which input to select.
              newConcatDimSize = b.create<arith::AddIOp>(
                  loc, concatDimSize, b.create<tensor::DimOp>(loc, arg, dim));
              Value cmp = b.create<arith::CmpIOp>(loc, rewriter.getI1Type(),
                                                  arith::CmpIPredicate::ult,
                                                  indexOp, newConcatDimSize);
              ifOp = b.create<scf::IfOp>(loc, resultType.getElementType(), cmp,
                                         true);
              if (result) {
                b.create<scf::YieldOp>(loc, ifOp->getResults()[0]);
              } else {
                result = ifOp->getResults()[0];
              }

              b = ifOp.getThenBodyBuilder(b.getListener());
            }

            // Now adjust the index for the concatenated dimension to fit into
            // the selected tensor and do an extract at that position.
            extractIndices[dim] =
                b.create<arith::SubIOp>(loc, indexOp, concatDimSize);
            Value extract =
                b.create<tensor::ExtractOp>(loc, arg, extractIndices);
            b.create<scf::YieldOp>(loc, extract);

            if (ifOp) {
              b = ifOp.getElseBodyBuilder(b.getListener());
              concatDimSize = newConcatDimSize;
            }
          }
          nestedBuilder.create<linalg::YieldOp>(loc, result);
        },
        pruneAttributeList(op));
    return success();
  }
};

class ConstConverterTensor : public OpConversionPattern<mhlo::ConstOp> {
 public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ConstOp constOp, OpAdaptor /*adaptor*/,
      ConversionPatternRewriter& rewriter) const final {
    auto valueAttr = constOp.value().cast<DenseElementsAttr>();
    auto type =
        typeConverter->convertType(constOp.getType()).cast<ShapedType>();
    if (type != constOp.getType()) {
      // Signedness conversion.
      valueAttr = valueAttr.mapValues(type.getElementType(),
                                      [](const APInt& i) { return i; });
    }
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(constOp, type, valueAttr);
    return success();
  }
};

// TODO(b/156787842): Support the lowering for dynamic shapes.
class ReverseConverter
    : public DataMovementOpConverter<ReverseConverter, mhlo::ReverseOp> {
 public:
  using DataMovementOpConverter<ReverseConverter,
                                mhlo::ReverseOp>::DataMovementOpConverter;
  static SmallVector<AffineMap, 2> getIndexingMaps(mhlo::ReverseOp op,
                                                   Builder* b) {
    auto resultType = getHloOpResultType(op).cast<ShapedType>();
    auto nloops = resultType.getRank();
    SmallVector<AffineExpr, 2> inputExprs;
    inputExprs.reserve(nloops);
    for (int i = 0; i < nloops; ++i)
      inputExprs.push_back(b->getAffineDimExpr(i));
    for (auto dim : op.dimensions()) {
      int i = dim.getZExtValue();
      if (resultType.isDynamicDim(i)) return {};
      int n = resultType.getShape()[i];
      inputExprs[i] = b->getAffineConstantExpr(n - 1) - inputExprs[i];
    }
    return {
        AffineMap::get(nloops, /*symbolCount=*/0, inputExprs, b->getContext()),
        b->getMultiDimIdentityMap(nloops)};
  }
};

class SliceConverter : public OpConversionPattern<mhlo::SliceOp> {
 public:
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::SliceOp sliceOp, typename mhlo::SliceOp::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto argType = adaptor.getOperands()[0].getType().dyn_cast<ShapedType>();
    if (!argType || !argType.hasRank()) {
      return rewriter.notifyMatchFailure(sliceOp, "expects known-rank args");
    }

    SmallVector<OpFoldResult, 3> offsets, sizes, strides;
    for (int i = 0, e = argType.getRank(); i < e; ++i) {
      auto start = sliceOp.start_indices().getValues<int64_t>()[i];
      auto limit = sliceOp.limit_indices().getValues<int64_t>()[i];
      auto stride = sliceOp.strides().getValues<int64_t>()[i];
      offsets.push_back(rewriter.getI64IntegerAttr(start));
      // Say that there are k elements in total, we have condition:
      //   start + (k - 1) * strides <= limit - 1
      // ->
      //   k <= (limit - 1 - start) / strides + 1
      sizes.push_back(
          rewriter.getI64IntegerAttr((limit - 1 - start) / stride + 1));
      strides.push_back(rewriter.getI64IntegerAttr(stride));
    }
    rewriter.replaceOpWithNewOp<tensor::ExtractSliceOp>(
        sliceOp, adaptor.getOperands()[0], offsets, sizes, strides);
    return success();
  }
};

class DynamicSliceConverter : public OpConversionPattern<mhlo::DynamicSliceOp> {
 public:
  using OpConversionPattern<mhlo::DynamicSliceOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::DynamicSliceOp dynamicSliceOp, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto loc = dynamicSliceOp.getLoc();
    auto argType = adaptor.operand().getType().dyn_cast<ShapedType>();
    if (!argType || !argType.hasRank()) {
      return rewriter.notifyMatchFailure(dynamicSliceOp,
                                         "require known-rank args");
    }

    auto indexType = rewriter.getIndexType();
    SmallVector<OpFoldResult, 3> startIndices, sizes;
    Value zero = rewriter.create<arith::ConstantOp>(
        loc, rewriter.getZeroAttr(adaptor.start_indices()[0]
                                      .getType()
                                      .cast<RankedTensorType>()
                                      .getElementType()));
    for (auto& en : llvm::enumerate(
             llvm::zip(adaptor.start_indices(),
                       dynamicSliceOp.slice_sizes().getValues<int64_t>()))) {
      int64_t size = std::get<1>(en.value());
      sizes.push_back(rewriter.getI64IntegerAttr(size));

      // By mhlo.DynamicSlice definition:
      //   `start_indices[i] = clamp(start_indices[i],
      //       0, operand.dimension_size[i] - size_indices[i])`
      Value startIndex =
          rewriter.create<tensor::ExtractOp>(loc, std::get<0>(en.value()));
      Value ub = rewriter.createOrFold<tensor::DimOp>(loc, adaptor.operand(),
                                                      en.index());
      // ClampOp lowering does not support index type, so cast it into integer
      // type.
      ub = rewriter.createOrFold<arith::IndexCastOp>(loc, startIndex.getType(),
                                                     ub);
      ub = rewriter.createOrFold<arith::SubIOp>(
          loc, ub,
          rewriter.create<arith::ConstantOp>(
              loc, rewriter.getIntegerAttr(startIndex.getType(), size)));
      startIndex = mhlo::MhloOpToStdScalarOp::map<mhlo::ClampOp>(
          loc, startIndex.getType(),
          ArrayRef<Type>{startIndex.getType(), startIndex.getType(),
                         startIndex.getType()},
          ArrayRef<Value>{zero, startIndex, ub}, &rewriter);
      startIndices.push_back(
          rewriter.create<arith::IndexCastOp>(loc, indexType, startIndex)
              .getResult());
    }

    int64_t rank = argType.getRank();
    SmallVector<OpFoldResult, 3> strides(rank, rewriter.getI64IntegerAttr(1));

    auto resultType = this->typeConverter->convertType(dynamicSliceOp.getType())
                          .cast<RankedTensorType>();

    rewriter.replaceOpWithNewOp<tensor::ExtractSliceOp>(
        dynamicSliceOp, resultType, adaptor.operand(), startIndices, sizes,
        strides);
    return success();
  }
};

class DynamicUpdateSliceConverter
    : public OpConversionPattern<mhlo::DynamicUpdateSliceOp> {
 public:
  using OpConversionPattern<mhlo::DynamicUpdateSliceOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::DynamicUpdateSliceOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    auto loc = op.getLoc();
    auto operandType = adaptor.operand().getType().dyn_cast<RankedTensorType>();
    if (!operandType || !operandType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(
          op, "require static ranked type for operand");
    }

    auto updateType = adaptor.update().getType().dyn_cast<RankedTensorType>();
    if (!updateType || !updateType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(
          op, "require static ranked type for operand");
    }

    // We do not have to clamp sizes because the semantic of `update`
    // guarantees that it is always in the bounds. See
    // https://www.tensorflow.org/xla/operation_semantics#dynamicupdateslice
    SmallVector<OpFoldResult, 3> sizes;
    for (auto size : updateType.getShape()) {
      sizes.push_back(rewriter.getIndexAttr(size));
    }

    auto indexType = rewriter.getIndexType();
    SmallVector<OpFoldResult, 3> startIndices;
    Type startIndexType = adaptor.start_indices()[0]
                              .getType()
                              .cast<RankedTensorType>()
                              .getElementType();
    Value zero = rewriter.create<arith::ConstantOp>(
        loc, rewriter.getZeroAttr(startIndexType));
    for (auto& en : llvm::enumerate(adaptor.start_indices())) {
      // By mhlo.DynamicUpdateSlice definition:
      //   `start_indices[i] = clamp(start_indices[i],
      //       0, operand.dimension_size[i] - update.dimension_size[i])`
      Value startIndex = rewriter.create<tensor::ExtractOp>(loc, en.value());
      Value ub = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getIntegerAttr(startIndexType,
                                       operandType.getDimSize(en.index()) -
                                           updateType.getDimSize(en.index())));
      startIndex = mhlo::MhloOpToStdScalarOp::map<mhlo::ClampOp>(
          loc, startIndexType,
          ArrayRef<Type>{startIndexType, startIndexType, startIndexType},
          ArrayRef<Value>{zero, startIndex, ub}, &rewriter);
      startIndices.push_back(
          rewriter.create<arith::IndexCastOp>(loc, indexType, startIndex)
              .getResult());
    }

    int64_t rank = operandType.getRank();
    SmallVector<OpFoldResult, 3> strides(rank, rewriter.getI64IntegerAttr(1));
    rewriter.replaceOpWithNewOp<tensor::InsertSliceOp>(
        op, adaptor.update(), adaptor.operand(), startIndices, sizes, strides);
    return success();
  }
};

enum class DotOperationType {
  kVectorDot = 0,
  kMatrixVector,
  kVectorMatrix,
  kMatrixMatrix,
  kUnsupported
};

DotOperationType getDotOperationType(mhlo::DotOp dotOp) {
  ArrayRef<int64_t> lhsShape =
      dotOp.lhs().getType().cast<ShapedType>().getShape();
  ArrayRef<int64_t> rhsShape =
      dotOp.rhs().getType().cast<ShapedType>().getShape();
  auto shapeMatches = [](int64_t a, int64_t b) {
    return a == ShapedType::kDynamicSize || b == ShapedType::kDynamicSize ||
           a == b;
  };
  if (lhsShape.size() == 1 && rhsShape.size() == 1 &&
      shapeMatches(lhsShape[0], rhsShape[0])) {
    return DotOperationType::kVectorDot;
  }
  if (lhsShape.size() == 2 && rhsShape.size() == 1 &&
      shapeMatches(lhsShape[1], rhsShape[0])) {
    return DotOperationType::kMatrixVector;
  }
  if (lhsShape.size() == 1 && rhsShape.size() == 2 &&
      shapeMatches(lhsShape[0], rhsShape[0])) {
    return DotOperationType::kVectorMatrix;
  }
  if (lhsShape.size() == 2 && rhsShape.size() == 2 &&
      shapeMatches(lhsShape[1], rhsShape[0])) {
    return DotOperationType::kMatrixMatrix;
  }
  return DotOperationType::kUnsupported;
}

SmallVector<Value, 2> getDotOpInitTensorDynSizes(OpBuilder& b, Location loc,
                                                 Value lhs, Value rhs,
                                                 DotOperationType type) {
  SmallVector<Value, 2> dynShape;
  switch (type) {
    case DotOperationType::kMatrixMatrix: {
      if (lhs.getType().cast<ShapedType>().isDynamicDim(0))
        dynShape.push_back(b.create<tensor::DimOp>(loc, lhs, 0));
      if (rhs.getType().cast<ShapedType>().isDynamicDim(1))
        dynShape.push_back(b.create<tensor::DimOp>(loc, rhs, 1));
      break;
    }
    case DotOperationType::kMatrixVector: {
      if (lhs.getType().cast<ShapedType>().isDynamicDim(0))
        dynShape.push_back(b.create<tensor::DimOp>(loc, lhs, 0));
      break;
    }
    case DotOperationType::kVectorMatrix: {
      if (rhs.getType().cast<ShapedType>().isDynamicDim(1))
        dynShape.push_back(b.create<tensor::DimOp>(loc, rhs, 1));
      break;
    }
    case DotOperationType::kVectorDot:
    case DotOperationType::kUnsupported:
    default: {
      break;
    }
  }
  return dynShape;
}

template <DotOperationType op_type, typename LinalgOp>
class DotOpConversion : public OpConversionPattern<mhlo::DotOp> {
 public:
  using OpConversionPattern<mhlo::DotOp>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::DotOp op, mhlo::DotOp::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!verifyHloOpBufferOrTensorSemantics(op)) {
      return failure();
    }
    if (getDotOperationType(op) != op_type) return failure();

    Location loc = op.getLoc();
    // Convert unsigned to signed. This works because signed and unsigned
    // integer matmul is the same operation in two's complement.
    auto outputType =
        typeConverter->convertType(op.getType()).cast<ShapedType>();
    SmallVector<Value, 2> dynShape = getDotOpInitTensorDynSizes(
        rewriter, loc, adaptor.lhs(), adaptor.rhs(), op_type);
    auto initTensor = getInitTensor(rewriter, loc, outputType, dynShape);
    Value zeroTensor = fillTensorWithZeros(rewriter, loc, initTensor);
    rewriter.replaceOpWithNewOp<LinalgOp>(
        op, TypeRange{outputType}, ValueRange{adaptor.lhs(), adaptor.rhs()},
        ValueRange{zeroTensor}, pruneAttributeList(op));
    return success();
  }
};

class DotGeneralBatchMatMulOpConversion
    : public OpConversionPattern<mhlo::DotGeneralOp> {
 public:
  using OpConversionPattern<mhlo::DotGeneralOp>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::DotGeneralOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!verifyHloOpBufferOrTensorSemantics(op)) {
      return failure();
    }
    if (op.getType().cast<RankedTensorType>().getRank() != 3) {
      return rewriter.notifyMatchFailure(op, "expected a batch matmul");
    }

    mhlo::DotDimensionNumbersAttr dimNumbers = op.dot_dimension_numbers();
    auto lhsBatchingDims = dimNumbers.getLhsBatchingDimensions();
    auto rhsBatchingDims = dimNumbers.getRhsBatchingDimensions();
    auto lhsContractingDims = dimNumbers.getLhsContractingDimensions();
    auto rhsContractingDims = dimNumbers.getRhsContractingDimensions();
    if (lhsBatchingDims.size() != 1 || lhsBatchingDims[0] != 0) {
      return rewriter.notifyMatchFailure(
          op, "expected lhs batching dimensions exactly {0}");
    }
    if (rhsBatchingDims.size() != 1 || rhsBatchingDims[0] != 0) {
      return rewriter.notifyMatchFailure(
          op, "expected rhs batching dimensions exactly {0}");
    }
    if (lhsContractingDims.size() != 1 || lhsContractingDims[0] != 2) {
      return rewriter.notifyMatchFailure(
          op, "expected lhs contracting dimensions exactly {2}");
    }
    if (rhsContractingDims.size() != 1 || rhsContractingDims[0] != 1) {
      return rewriter.notifyMatchFailure(
          op, "expected rhs contracting dimensions exactly {1}");
    }

    Location loc = op.getLoc();
    // Convert unsigned to signed. This works because signed and unsigned
    // integer matmul is the same operation in two's complement.
    auto outputType =
        typeConverter->convertType(op.getType()).cast<ShapedType>();
    auto initTensor =
        getInitTensorFor(rewriter, loc, outputType, op, adaptor.getOperands());
    Value zeroTensor = fillTensorWithZeros(rewriter, loc, initTensor);
    Operation* linalgOp = rewriter.create<linalg::BatchMatmulOp>(
        loc, /*resultTensorTypes=*/TypeRange{outputType},
        /*inputs=*/ValueRange{adaptor.lhs(), adaptor.rhs()},
        /*outputBuffers=*/ValueRange{zeroTensor}, pruneAttributeList(op));

    rewriter.replaceOp(op, linalgOp->getResults());
    return success();
  }
};

class MapOpConverter : public OpConversionPattern<mhlo::MapOp> {
 public:
  using OpConversionPattern::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::MapOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!verifyHloOpBufferOrTensorSemantics(op)) return failure();

    auto resultType =
        typeConverter->convertType(op.getType()).cast<ShapedType>();
    assert(op.dimensions().size() == resultType.getRank() &&
           "Expected a pointwise map");

    Location loc = op.getLoc();
    Value output =
        getInitTensorFor(rewriter, loc, resultType, op, adaptor.getOperands());
    SmallVector<AffineMap> indexingMaps(
        op.getNumOperands() + 1,
        rewriter.getMultiDimIdentityMap(resultType.getRank()));

    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, resultType, adaptor.getOperands(), output, indexingMaps,
        getNParallelLoopsAttrs(resultType.getRank()),
        /*bodyBuild=*/nullptr, pruneAttributeList(op));

    // Convert the signature of the body. We scalarize the operands and add a
    // scalar operand representing the output tensor.
    Region& region = linalgOp.region();
    rewriter.inlineRegionBefore(op.computation(), region, region.end());
    TypeConverter::SignatureConversion signatureConverter(op.getNumOperands() +
                                                          1);

    for (const auto& it : llvm::enumerate(op.getOperation()->getOperands())) {
      signatureConverter.addInputs(
          it.index(),
          typeConverter->convertType(
              it.value().getType().cast<ShapedType>().getElementType()));
    }
    signatureConverter.addInputs(resultType.getElementType());

    rewriter.applySignatureConversion(&region, signatureConverter);
    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }
};

bool isInBodyOfLinalgOps(Operation* op) {
  auto* parentOp = op->getParentRegion()->getParentOp();
  return parentOp->getDialect() ==
         parentOp->getContext()->getLoadedDialect<linalg::LinalgDialect>();
}

template <typename OpTy>
struct ReduceRegionXLAOpConversion : public OpConversionPattern<OpTy> {
  using OpConversionPattern<OpTy>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      OpTy op, typename OpTy::Adaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!isInBodyOfLinalgOps(op)) {
      return failure();
    }
    if (!op.getResult().getType().template isa<TensorType>()) return failure();
    if (llvm::all_of(adaptor.getOperands(), [](Value arg) {
          return arg.getType().template isa<TensorType>();
        })) {
      return failure();
    }
    // RemoveSignTypeConverter would give us a tensor. We also have to scalarize
    // so do it manually.
    Type resultType = getElementTypeOrSelf(op.getType());
    if (resultType.isUnsignedInteger()) {
      resultType = IntegerType::get(resultType.getContext(),
                                    resultType.getIntOrFloatBitWidth());
    }
    // The scalar mapper has to know the original type. At this point the
    // operands have been converted from `tensor<ui32>` to `i32` so recreate
    // `ui32` from the original operands.
    auto operandTypes = llvm::to_vector(llvm::map_range(
        op->getOperandTypes(), [](Type t) { return getElementTypeOrSelf(t); }));
    Value result = mhlo::MhloOpToStdScalarOp::map<OpTy>(
        op, resultType, operandTypes, adaptor.getOperands(), &rewriter);
    rewriter.replaceOp(op, result);
    return success();
  }
};

SmallVector<Value, 8> getReduceOpInitTensorDynSizes(
    OpBuilder& b, Location loc, Value arg, ShapedType resultType,
    ArrayRef<int64_t> reductionDims) {
  llvm::SmallSetVector<int, 4> s;
  for (auto dim : reductionDims) s.insert(dim);

  SmallVector<unsigned, 4> parallelDims;
  SmallVector<Value, 8> dynShape;
  int rank = arg.getType().cast<RankedTensorType>().getRank();
  for (int i = 0, j = 0; i < rank; ++i) {
    if (s.count(i)) continue;
    if (!resultType.isDynamicDim(j++)) continue;
    dynShape.push_back(b.create<tensor::DimOp>(loc, arg, i));
  }

  return dynShape;
}

class ReduceRegionReturnOpConversion
    : public OpConversionPattern<mhlo::ReturnOp> {
 public:
  using OpConversionPattern<mhlo::ReturnOp>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::ReturnOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!isInBodyOfLinalgOps(op)) {
      return failure();
    }
    SmallVector<Value, 4> operands(adaptor.getOperands());
    for (size_t i = 0; i < operands.size(); ++i) {
      if (operands[i].getType().isa<ShapedType>()) {
        auto loc = operands[i].getLoc();
        operands[i] = rewriter.create<tensor::ExtractOp>(loc, operands[i]);
      }
    }
    rewriter.replaceOpWithNewOp<linalg::YieldOp>(op, operands);
    return success();
  }
};

class ReduceConversion : public OpConversionPattern<mhlo::ReduceOp> {
 public:
  using OpConversionPattern<mhlo::ReduceOp>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::ReduceOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    Location loc = op.getLoc();

    int numOperands = static_cast<int>(adaptor.operands().size());

    if (llvm::any_of(adaptor.operands(), [](Value v) {
          return !v.getType().cast<ShapedType>().getRank();
        })) {
      return rewriter.notifyMatchFailure(op, "expects known-rank args");
    }
    auto srcRank = adaptor.operands()[0].getType().cast<ShapedType>().getRank();

    SmallVector<int64_t, 4> reductionDims = extract1DVector(op.dimensions());

    SmallVector<Type> resultTypes;
    if (failed(typeConverter->convertTypes(op.getResultTypes(), resultTypes)))
      return failure();

    SmallVector<Value> operands, outputs;
    SmallVector<AffineMap, 3> indexingMaps;
    for (auto values :
         llvm::zip(adaptor.operands(), adaptor.init_values(), resultTypes)) {
      // Check if init_value is constant. If so, inline the value into the
      // region.
      Value operand = std::get<0>(values);
      Value initValue = std::get<1>(values);
      Type resultType = std::get<2>(values);
      initValue = rewriter.createOrFold<tensor::ExtractOp>(loc, initValue);

      operands.push_back(operand);
      SmallVector<Value, 8> dynShape = getReduceOpInitTensorDynSizes(
          rewriter, loc, operand, resultType, reductionDims);
      auto initTensor = getInitTensor(rewriter, loc, resultType, dynShape);
      Value filledTensor =
          rewriter.create<linalg::FillOp>(loc, initValue, initTensor).result();
      outputs.push_back(filledTensor);
    }

    // Prepare indexing maps for linalg generic op. The elements are for src
    // and dst. Transpose `src` to make the reduction loops be the innermost,
    // because it's easier to fully utilize processors.
    indexingMaps.append(
        numOperands, getTransposeMapForReduction(rewriter.getContext(),
                                                 (int)srcRank, reductionDims));

    // The indexing map of `dst` should drop the reduction loops. Since the
    // reduction loops now are all in the innermost, drops
    // `reduction_dims.size()` dimensions. We don't need an inverse
    // permutation here because they are the same.
    SmallVector<AffineExpr, 4> exprs;
    for (int i = 0, e = srcRank - reductionDims.size(); i < e; ++i)
      exprs.push_back(rewriter.getAffineDimExpr(i));
    indexingMaps.append(numOperands,
                        AffineMap::get(srcRank, /*symbolCount=*/0, exprs,
                                       rewriter.getContext()));

    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, /*resultTensorTypes=*/resultTypes, operands,
        /*outputBuffers=*/ValueRange{outputs}, indexingMaps,
        getParallelAndReductionIterators(srcRank, reductionDims.size()),
        /*bodyBuild=*/nullptr, pruneAttributeList(op));

    // Convert the signature of the body. The reduce op region apply function
    // has a signature (lhs, rhs) -> output, all of the same tensor type t.
    // This is converted to a function with the same signature but with
    // element types. E.g., "(tensor<f32>, tensor<f32>) -> tensor<f32>" will
    // be converted to "(f32, f32, f32)".
    Region& region = linalgOp.region();
    rewriter.inlineRegionBefore(op.body(), region, region.end());
    TypeConverter::SignatureConversion signatureConverter(numOperands * 2);

    // map operand and init values's types
    for (const auto& it : llvm::enumerate(op.getOperation()->getOperands())) {
      signatureConverter.addInputs(
          it.index(),
          typeConverter->convertType(
              it.value().getType().cast<ShapedType>().getElementType()));
    }

    rewriter.applySignatureConversion(&region, signatureConverter);
    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }
};

// Decomposes a pad with negative edge padding into a pad without negative edge
// padding and a tensor.extract_slice.
struct PadOpNegativePaddingConversion
    : public OpConversionPattern<mhlo::PadOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::PadOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    SmallVector<int64_t, 4> pad_low;
    SmallVector<int64_t, 4> pad_high;
    SmallVector<OpFoldResult, 4> slice_starts;

    bool hasNegativePadding = false;
    for (int64_t low : op.edge_padding_low().getValues<int64_t>()) {
      if (low >= 0) {
        pad_low.push_back(low);
        slice_starts.push_back(rewriter.getIndexAttr(0));
      } else {
        pad_low.push_back(0);
        slice_starts.push_back(rewriter.getIndexAttr(-low));
        hasNegativePadding = true;
      }
    }

    for (int64_t high : op.edge_padding_high().getValues<int64_t>()) {
      if (high >= 0) {
        pad_high.push_back(high);
      } else {
        pad_high.push_back(-high);
        hasNegativePadding = true;
      }
    }

    // If there's no negative edge padding we're done.
    if (!hasNegativePadding) return failure();

    // Create a new pad op with the positive values.
    Value pad = rewriter.create<mhlo::PadOp>(
        op.getLoc(), adaptor.operand(), adaptor.padding_value(),
        rewriter.getI64TensorAttr(pad_low), rewriter.getI64TensorAttr(pad_high),
        op.interior_padding());

    // Then slice according to the negative edge padding. Static shapes only for
    // now.
    if (!op.getType().hasStaticShape()) return failure();
    SmallVector<OpFoldResult, 4> sizes(llvm::map_range(
        op.getType().getShape(),
        [&](int64_t dim) { return rewriter.getIndexAttr(dim); }));
    SmallVector<OpFoldResult, 4> strides(slice_starts.size(),
                                         rewriter.getIndexAttr(1));
    rewriter.replaceOpWithNewOp<tensor::ExtractSliceOp>(op, pad, slice_starts,
                                                        sizes, strides);
    return success();
  }
};

/// Converts mhlo.pad operation to tensor.pad or tensor.insert_slice.
struct PadOpConversion : public OpConversionPattern<mhlo::PadOp> {
  using OpConversionPattern<mhlo::PadOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::PadOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    auto loc = op.getLoc();
    auto resultType = typeConverter->convertType(op.getResult().getType());

    // Negative edge padding is decomposed separately.
    auto isNegative = [](const APInt& intVal) { return intVal.isNegative(); };
    if (llvm::any_of(op.edge_padding_low().getValues<APInt>(), isNegative) ||
        llvm::any_of(op.edge_padding_high().getValues<APInt>(), isNegative))
      return failure();

    Value paddingVal =
        rewriter.createOrFold<tensor::ExtractOp>(loc, adaptor.padding_value());

    SmallVector<OpFoldResult, 4> low(
        op.edge_padding_low().getValues<IntegerAttr>());

    // If there is no interior padding lower to tensor.pad directly.
    if (llvm::all_of(op.interior_padding().getValues<APInt>(),
                     [](const APInt& intVal) { return intVal.isZero(); })) {
      SmallVector<OpFoldResult, 4> high(
          op.edge_padding_high().getValues<IntegerAttr>());
      auto padTensorOp = tensor::createPadScalarOp(
          resultType, adaptor.operand(), paddingVal, low, high,
          /*nofold=*/false, loc, rewriter);
      rewriter.replaceOp(op, padTensorOp.getResult());
      return success();
    }

    // We have interior padding, which can be lowered to tensor.insert_slice.
    // Start by filling a result-sized tensor with the pad value.
    auto initTensor =
        getInitTensorFor(rewriter, loc, resultType, op, adaptor.getOperands());
    auto fill =
        rewriter.create<linalg::FillOp>(loc, paddingVal, initTensor).result();

    // Get sizes of the original operand.
    auto operandType = adaptor.operand().getType().cast<ShapedType>();
    auto sizes = llvm::to_vector<4>(llvm::map_range(
        llvm::seq<int64_t>(0, operandType.getRank()),
        [&](int64_t dim) -> OpFoldResult {
          if (!operandType.isDynamicDim(dim))
            return rewriter.getIndexAttr(operandType.getDimSize(dim));
          return rewriter.create<tensor::DimOp>(loc, adaptor.operand(), dim)
              .result();
        }));
    // Map interior padding to strides.
    auto strides = llvm::to_vector<4>(
        llvm::map_range(op.interior_padding().getValues<IntegerAttr>(),
                        [&](IntegerAttr stride) -> OpFoldResult {
                          return rewriter.getIntegerAttr(stride.getType(),
                                                         stride.getValue() + 1);
                        }));

    rewriter.replaceOpWithNewOp<tensor::InsertSliceOp>(
        op, adaptor.operand(), fill, low, sizes, strides);
    return success();
  }
};

// Apply dilation and padding to the input of a convolution.
Value applyConvolutionPadding(Location loc, Value input,
                              DenseIntElementsAttr padding,
                              DenseIntElementsAttr lhsDilation,
                              OpBuilder& rewriter) {
  if ((!padding || isSplatValue(padding, 0)) &&
      (!lhsDilation || isSplatValue(lhsDilation, 1)))
    return input;

  auto inputType = input.getType().cast<ShapedType>();
  auto rank = inputType.getRank();

  // Translate window padding into low/high padding.
  SmallVector<int64_t, 8> padLow(rank, 0);
  SmallVector<int64_t, 8> padHigh(rank, 0);
  if (padding) {
    // The padding attribute contains two values per dimension, but excludes the
    // batch and feature dimensions.
    assert(rank * 2 == padding.size() + 4 &&
           "There should be 2 padding values per dimension, i.e low and high.");
    for (auto i : llvm::seq<int64_t>(0, padding.size() / 2)) {
      padLow[i + 1] = padding.getValues<int64_t>()[i * 2];
      padHigh[i + 1] = padding.getValues<int64_t>()[i * 2 + 1];
    }
  }

  // Translate input dilation into interior padding.
  SmallVector<int64_t, 8> padInterior(rank, 0);
  if (lhsDilation) {
    assert(rank == lhsDilation.size() + 2);
    for (auto i : llvm::seq<int64_t>(0, lhsDilation.size())) {
      padInterior[i + 1] = lhsDilation.getValues<int64_t>()[i] - 1;
    }
  }

  auto indexType = rewriter.getIndexType();
  auto attrType = RankedTensorType::get({rank}, indexType);
  Value zero = rewriter.create<arith::ConstantOp>(
      loc, rewriter.getZeroAttr(
               RankedTensorType::get({}, inputType.getElementType())));
  return rewriter.create<mhlo::PadOp>(
      loc, input, zero, DenseIntElementsAttr::get(attrType, padLow),
      DenseIntElementsAttr::get(attrType, padHigh),
      DenseIntElementsAttr::get(attrType, padInterior));
}

/// Converts mhlo.conv operation to linalg named op. This only covers normal
/// convolution cases. The op must have canonical dimension numbers. Depthwise
/// convolution and pointwise convolution are not handled in the conversion.
struct NormalConvOpConversion : public OpConversionPattern<mhlo::ConvOp> {
  using OpConversionPattern<mhlo::ConvOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ConvOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    if (!hasCanonicalDimensionNumbers(op.dimension_numbers())) return failure();
    if (op.feature_group_count() != 1u) return failure();

    Location loc = op.getLoc();
    Value input = adaptor.lhs();
    Value filter = adaptor.rhs();
    auto resultType =
        typeConverter->convertType(op.getResult().getType()).cast<ShapedType>();
    int64_t rank = resultType.getRank();

    // The output shape is N spatial_dims F.
    SmallVector<Value, 8> dynSizes;
    if (resultType.isDynamicDim(0)) {
      dynSizes.push_back(rewriter.create<tensor::DimOp>(loc, input, 0));
    }
    for (int64_t i = 1, e = rank - 1; i < e; ++i) {
      if (resultType.isDynamicDim(i)) {
        return rewriter.notifyMatchFailure(
            op, "expected output spatial dims to be static shapes");
      }
    }
    if (resultType.isDynamicDim(rank - 1)) {
      dynSizes.push_back(rewriter.create<tensor::DimOp>(loc, filter, rank - 1));
    }
    Value initTensor = rewriter.create<linalg::InitTensorOp>(
        loc, dynSizes, resultType.getShape(), resultType.getElementType());
    Value zeroTensor = fillTensorWithZeros(rewriter, loc, initTensor);
    linalg::LinalgOp res;
    Attribute strides = op.window_stridesAttr();
    Attribute dilations = op.rhs_dilationAttr();

    // Apply padding and input dilation.
    input = applyConvolutionPadding(loc, input, op.paddingAttr(),
                                    op.lhs_dilationAttr(), rewriter);

    switch (rank) {
      case 2: {
        res = rewriter.create<linalg::MatmulOp>(
            loc, resultType, ValueRange{input, filter}, ValueRange{zeroTensor},
            pruneAttributeList(op));
        break;
      }
      case 3: {
        res = rewriter.create<linalg::Conv1DNwcWcfOp>(
            loc, resultType, ValueRange{input, filter}, ValueRange{zeroTensor},
            strides, dilations, pruneAttributeList(op));
        break;
      }
      case 4: {
        res = rewriter.create<linalg::Conv2DNhwcHwcfOp>(
            loc, resultType, ValueRange{input, filter}, ValueRange{zeroTensor},
            strides, dilations, pruneAttributeList(op));
        break;
      }
      case 5: {
        res = rewriter.create<linalg::Conv3DNdhwcDhwcfOp>(
            loc, resultType, ValueRange{input, filter}, ValueRange{zeroTensor},
            strides, dilations, pruneAttributeList(op));
        break;
      }
      default:
        return rewriter.notifyMatchFailure(op, "expected 1/2/3D conv op");
    }
    rewriter.replaceOp(op, res.getOperation()->getResults());
    return success();
  }
};

/// Converts mhlo.convolution operation to
/// linalg.depthwise_conv_2d_input_nhwc_filter_hwcf op or
/// depthwise_conv_2d_input_nhwc_filter_hwc op.
struct DepthwiseConvOpConversion : public OpConversionPattern<mhlo::ConvOp> {
  using OpConversionPattern<mhlo::ConvOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ConvOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    if (op.batch_group_count() != 1) return failure();
    // Fall into the normal convolution cases.
    if (op.feature_group_count() == 1) return failure();

    const mhlo::ConvDimensionNumbersAttr& dimensionNumbers =
        op.dimension_numbers();
    // Make sure that this is 2-D convolution.
    const auto spatialRank =
        llvm::size(dimensionNumbers.getInputSpatialDimensions());
    if (spatialRank != 2) {
      return rewriter.notifyMatchFailure(op, "only support 2-D cases for now");
    }

    // Make sure that this is depthwise convolution.
    int64_t inputFeatureDim = dimensionNumbers.getInputFeatureDimension();
    int64_t inputFeatureCount =
        op.lhs().getType().cast<ShapedType>().getDimSize(inputFeatureDim);
    if (op.feature_group_count() != inputFeatureCount) {
      return rewriter.notifyMatchFailure(op, "not depth-wise convolution");
    }

    // Make sure that this convolution has a canonical form.
    if (!hasCanonicalDimensionNumbers(dimensionNumbers)) {
      return rewriter.notifyMatchFailure(op, "does not have canonical form");
    }

    DenseIntElementsAttr windowStrides;
    if (op.window_strides()) {
      windowStrides = op.window_strides().getValue();
    } else {
      windowStrides = rewriter.getI64VectorAttr({1, 1});
    }

    DenseIntElementsAttr rhsDilation;
    if (op.rhs_dilation()) {
      rhsDilation = op.rhs_dilation().getValue();
    } else {
      rhsDilation = rewriter.getI64VectorAttr({1, 1});
    }

    Location loc = op.getLoc();
    Value input = adaptor.lhs();
    Value filter = adaptor.rhs();
    auto resultType = typeConverter->convertType(op.getResult().getType())
                          .cast<RankedTensorType>();
    if (!resultType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(op,
                                         "expected output has static shapes");
    }

    // Apply padding and input dilation.
    input = applyConvolutionPadding(loc, input, op.paddingAttr(),
                                    op.lhs_dilationAttr(), rewriter);

    auto filterDims =
        llvm::to_vector<4>(op.rhs().getType().cast<ShapedType>().getShape());

    auto getIndicesVector = [](int start, int end) {
      return llvm::to_vector<2>(llvm::seq<int64_t>(start, end));
    };

    int64_t kernelInputFeatureDimension =
        dimensionNumbers.getKernelInputFeatureDimension();
    int64_t kernelOutputFeatureDimension =
        dimensionNumbers.getKernelOutputFeatureDimension();
    if (filterDims[kernelInputFeatureDimension] *
            filterDims[kernelOutputFeatureDimension] !=
        op.feature_group_count()) {
      // For cases where channel multiplier != 1

      // Reshaping filter shape
      //   [filter_height, filter_width, 1, kernel-output-feature].
      // to
      //   [filter_height, filter_width, feature_group_count,
      //      kernel-output-feature/feature_group_count ]
      SmallVector<int64_t> reshapedFilterDims;
      reshapedFilterDims.assign(filterDims.begin(), filterDims.end());
      auto reshapedFilter = filter;
      if (filterDims[kernelInputFeatureDimension] == 1) {
        reshapedFilterDims[kernelInputFeatureDimension] =
            op.feature_group_count();
        reshapedFilterDims[kernelOutputFeatureDimension] /=
            op.feature_group_count();
        auto reshapedFilterType = RankedTensorType::get(
            reshapedFilterDims,
            op.rhs().getType().cast<RankedTensorType>().getElementType());

        reshapedFilter =
            rewriter.create<mhlo::ReshapeOp>(loc, reshapedFilterType, filter);
      }

      auto outputDims = resultType.getShape();
      auto channelMultiplier = reshapedFilterDims[3];
      SmallVector<int64_t> reshapedOutputDims;
      reshapedOutputDims.assign(outputDims.begin(), outputDims.end());
      reshapedOutputDims.push_back(channelMultiplier);
      reshapedOutputDims[3] /= channelMultiplier;

      Value initTensor = rewriter.create<linalg::InitTensorOp>(
          loc, reshapedOutputDims, resultType.getElementType());
      Value zeroTensor = fillTensorWithZeros(rewriter, loc, initTensor);

      auto reshapedOutputType = RankedTensorType::get(
          reshapedOutputDims, resultType.getElementType());
      auto conv = rewriter.create<linalg::DepthwiseConv2DNhwcHwcmOp>(
          loc, reshapedOutputType, ValueRange{input, reshapedFilter},
          ValueRange{zeroTensor}, windowStrides, rhsDilation,
          pruneAttributeList(op));

      // Create a Linalg reshape op that converts the output from 5 dimensions
      // into 4 dimensions (by collapsing the last two dimensions). This is
      // needed because linalg.depthwise_conv_2d_input_nhwc_filter_hwcf returns
      // 5 dimensions for the output.
      SmallVector<ReassociationIndices, 4> collapsedDimList = {
          getIndicesVector(0, 1), getIndicesVector(1, 2),
          getIndicesVector(2, 3), getIndicesVector(3, 5)};
      rewriter.replaceOpWithNewOp<tensor::CollapseShapeOp>(
          op, resultType, conv.getResult(0), collapsedDimList);
    } else {
      // For cases where channel multiplier == 1
      Value initTensor = rewriter.create<linalg::InitTensorOp>(
          loc, resultType.getShape(), resultType.getElementType());
      Value zeroTensor = fillTensorWithZeros(rewriter, loc, initTensor);

      // Create a Linalg reshape op that converts the filter from 4 dimensions
      // into 3 dimensions (by droping the unit dimension). This is needed
      // because linalg.depthwise_conv_2d_input_nhwc_filter_hwc expects 3
      // dimensions for the filter.

      filterDims[2] = static_cast<int64_t>(op.feature_group_count());
      filterDims.pop_back();

      RankedTensorType filterShape =
          RankedTensorType::get(filterDims, op.getType().getElementType());

      SmallVector<ReassociationIndices, 4> collapsedDimList = {
          getIndicesVector(0, 1), getIndicesVector(1, 2),
          getIndicesVector(2, 4)};

      Value reshapedFilter = rewriter.create<tensor::CollapseShapeOp>(
          loc, filterShape, filter, collapsedDimList);

      rewriter.replaceOpWithNewOp<linalg::DepthwiseConv2DNhwcHwcOp>(
          op, resultType, ValueRange{input, reshapedFilter},
          ValueRange{zeroTensor}, windowStrides, rhsDilation,
          pruneAttributeList(op));
    }

    return success();
  }
};

struct ReduceWindowOpOnTensorsGenericConversion
    : public OpConversionPattern<mhlo::ReduceWindowOp> {
  using OpConversionPattern<mhlo::ReduceWindowOp>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::ReduceWindowOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    MLIRContext* ctx = op->getContext();
    Location loc = op.getLoc();
    llvm::SmallVector<Value> initValues = adaptor.init_values();
    llvm::SmallVector<Type> resultTypes = llvm::to_vector(op.getResultTypes());
    auto numOperands = initValues.size();

    llvm::SmallVector<int64_t> windowDimensions =
        extract1DVector(op.window_dimensions());

    llvm::SmallVector<int64_t> padding;
    if (op.padding()) {
      padding = extract1DVector(*op.padding());
    }

    llvm::SmallVector<int64_t> baseDilations;
    if (op.base_dilations()) {
      baseDilations = extract1DVector(*op.base_dilations());
    }

    llvm::SmallVector<int64_t> windowStrides(windowDimensions.size(), 1);
    if (op.window_strides()) {
      windowStrides = extract1DVector(*op.window_strides());
    }

    llvm::SmallVector<int64_t> windowDilations(windowDimensions.size(), 1);
    if (op.window_dilations()) {
      windowDilations = extract1DVector(*op.window_dilations());
    }

    auto rank = static_cast<int64_t>(windowDimensions.size());
    SmallVector<AffineExpr, 2> srcExprs;
    SmallVector<AffineExpr, 2> windowExprs;
    SmallVector<AffineExpr, 2> dstExprs;
    SmallVector<int64_t> filteredWindowDims;

    int windowDim = 0;
    for (int64_t i = 0; i < rank; i++) {
      AffineExpr srcExpr = mlir::getAffineDimExpr(i, ctx);

      if (windowStrides[i] != 1) srcExpr = srcExpr * windowStrides[i];

      if (windowDimensions[i] != 1) {
        filteredWindowDims.push_back(windowDimensions[i]);
        AffineExpr windowExpr = mlir::getAffineDimExpr(rank + windowDim, ctx);
        windowExprs.push_back(windowExpr);

        if (windowDilations[i] != 1)
          windowExpr = windowExpr * windowDilations[i];

        srcExpr = srcExpr + windowExpr;
        windowDim++;
      }

      srcExprs.push_back(srcExpr);
      dstExprs.push_back(mlir::getAffineDimExpr(i, ctx));
    }

    SmallVector<AffineMap, 4> inferredMaps =
        AffineMap::inferFromExprList({srcExprs, windowExprs, dstExprs});

    SmallVector<AffineMap, 4> indexingMaps;

    indexingMaps.append(numOperands, inferredMaps[0]);
    indexingMaps.append(1, inferredMaps[1]);
    indexingMaps.append(numOperands, inferredMaps[2]);

    // Setup the initial values.
    llvm::SmallVector<Value> broadcastValues;
    for (uint64_t i = 0, s = initValues.size(); i < s; i++) {
      Value initValue = initValues[i];
      auto resultTy = resultTypes[i].cast<ShapedType>();
      if (!resultTy.hasStaticShape()) return failure();

      auto broadcastSizes = rewriter.getI64TensorAttr(resultTy.getShape());
      broadcastValues.push_back(rewriter.create<mhlo::BroadcastOp>(
          loc, resultTy, initValue, broadcastSizes));
    }

    llvm::SmallVector<Value> inputs = llvm::to_vector(adaptor.operands());

    // Pad as necessary.
    if (llvm::any_of(padding, [](int64_t v) { return v != 0; }) ||
        llvm::any_of(baseDilations, [](int64_t v) { return v != 1; })) {
      llvm::SmallVector<int64_t> staticLows(rank, 0);
      llvm::SmallVector<int64_t> staticHighs(rank, 0);
      for (int i = 0; i < padding.size(); i += 2) {
        staticLows[i / 2] = padding[i];
        staticHighs[i / 2] = padding[i + 1];
      }
      // Translate base dilation into interior padding.
      llvm::SmallVector<int64_t> staticInteriors(rank, 0);
      for (const auto& dilation : llvm::enumerate(baseDilations)) {
        staticInteriors[dilation.index()] = dilation.value() - 1;
      }

      auto padAttrType = RankedTensorType::get({rank}, rewriter.getIndexType());
      auto padLows = DenseIntElementsAttr::get(padAttrType, staticLows);
      auto padHighs = DenseIntElementsAttr::get(padAttrType, staticHighs);
      auto padInteriors =
          DenseIntElementsAttr::get(padAttrType, staticInteriors);

      for (auto values : llvm::zip(inputs, initValues)) {
        auto& input = std::get<0>(values);
        auto& initValue = std::get<1>(values);
        input = rewriter.create<mhlo::PadOp>(loc, input, initValue, padLows,
                                             padHighs, padInteriors);
      }
    }

    // Add the extra input for the reduction dimension.
    inputs.push_back(rewriter.create<linalg::InitTensorOp>(
        loc, filteredWindowDims, rewriter.getF32Type()));

    rewriter.setInsertionPoint(op);
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, /*resultTensors=*/resultTypes,
        /*inputs=*/inputs,
        /*outputs=*/broadcastValues, indexingMaps,
        getParallelAndReductionIterators(rank + filteredWindowDims.size(),
                                         filteredWindowDims.size()),
        /*bodyBuild=*/nullptr, pruneAttributeList(op));

    // Convert the signature of the body. This includes converting scalar
    // tensors to their scalar values and inserting an additional block arg for
    // the window arg.
    Region& region = linalgOp.region();
    rewriter.cloneRegionBefore(op.body(), region, region.end());

    TypeConverter::SignatureConversion signatureConverter(
        inputs.size() + op->getNumResults() - 1);

    for (uint64_t i = 0, s = inputs.size(); i < s - 1; i++) {
      signatureConverter.addInputs(
          i, inputs[i].getType().cast<ShapedType>().getElementType());
    }

    signatureConverter.addInputs(
        inputs.back().getType().cast<ShapedType>().getElementType());

    for (uint64_t i = 0, s = resultTypes.size(); i < s; i++) {
      auto idx = inputs.size() + i - 1;
      signatureConverter.addInputs(
          idx, resultTypes[i].cast<ShapedType>().getElementType());
    }

    rewriter.applySignatureConversion(&region, signatureConverter);
    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }
};

struct ReduceWindowOpConversion
    : public OpConversionPattern<mhlo::ReduceWindowOp> {
  using OpConversionPattern<mhlo::ReduceWindowOp>::OpConversionPattern;

  /// mhlo.reduce_window is mapped to a linalg.pooling operation. The type of
  /// the pooling is determined based on the body of the reduce window
  /// operation. This class enumerates the different variants.
  enum class PoolingType {
    kInvalid,
    k2DMin,
    k3DMin,
    k2DMax,
    k3DMax,
    k2DAdd,
    k3DAdd,
  };

  static PoolingType getPoolingType(mhlo::ReduceWindowOp reduceOp,
                                    int resultIndex) {
    auto rank =
        reduceOp.getResultTypes()[resultIndex].cast<ShapedType>().getRank();
    if (Operation* op = reduceOp.getReductionOp(resultIndex)) {
      if (isa<mhlo::MinOp>(*op) && rank == 4) return PoolingType::k2DMin;
      if (isa<mhlo::MinOp>(*op) && rank == 5) return PoolingType::k3DMin;
      if (isa<mhlo::MaxOp>(*op) && rank == 4) return PoolingType::k2DMax;
      if (isa<mhlo::MaxOp>(*op) && rank == 5) return PoolingType::k3DMax;
      if (isa<mhlo::AddOp>(*op) && rank == 4) return PoolingType::k2DAdd;
      if (isa<mhlo::AddOp>(*op) && rank == 5) return PoolingType::k3DAdd;
    }
    return PoolingType::kInvalid;
  }

  LogicalResult matchAndRewrite(
      mhlo::ReduceWindowOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const override {
    auto loc = op.getLoc();
    int rank = op.getResultTypes()[0].cast<ShapedType>().getRank();
    if (rank != 4 && rank != 5) {
      return rewriter.notifyMatchFailure(
          op, "expected NHWC/NDHWC pooling-based op");
    }

    if (op.padding() && !isSplatValue(*op.padding(), 0)) {
      return rewriter.notifyMatchFailure(op, "require paddings are all zero");
    }

    int lastDim = rank - 1;
    SmallVector<int64_t, 2> fakeWindowShapes;
    for (int i = 1; i < lastDim; ++i) {
      fakeWindowShapes.push_back(
          op.window_dimensions().getValues<int64_t>()[i]);
    }

    if (op.window_strides() &&
        (op.window_strides().getValue().getValues<int64_t>()[0] != 1 ||
         op.window_strides().getValue().getValues<int64_t>()[lastDim] != 1)) {
      return rewriter.notifyMatchFailure(
          op, "expected window_strides to be [1,x,y,(z),1]");
    }
    if (op.window_dimensions() &&
        (op.window_dimensions().getValues<int64_t>()[0] != 1 ||
         op.window_dimensions().getValues<int64_t>()[lastDim] != 1)) {
      return rewriter.notifyMatchFailure(
          op, "expected window_dimensions to be [1,x,y,(z),1]");
    }

    Attribute strides;
    SmallVector<int64_t> vec;
    if (op.window_stridesAttr()) {
      for (int i = 1; i < lastDim; ++i) {
        vec.push_back(op.window_strides().getValue().getValues<int64_t>()[i]);
      }
    } else {
      vec.assign(rank - 2, 1);
    }
    strides = rewriter.getI64VectorAttr(vec);

    Attribute dilations;
    vec.clear();
    if (op.window_dilations()) {
      for (int i = 1; i < lastDim; ++i) {
        vec.push_back(op.window_dilations().getValue().getValues<int64_t>()[i]);
      }
    } else {
      vec.assign(rank - 2, 1);
    }
    dilations = rewriter.getI64VectorAttr(vec);

    SmallVector<Value> poolingOps;

    ValueRange operands = adaptor.operands();
    ValueRange initValues = adaptor.init_values();
    for (auto it : llvm::zip(op.getResults(), operands, initValues)) {
      OpResult result = std::get<0>(it);
      Value input = std::get<1>(it);
      Value initValue = std::get<2>(it);
      auto resultType = result.getType().cast<ShapedType>();
      if (!input.getType().cast<ShapedType>().getElementType().isF32()) {
        return rewriter.notifyMatchFailure(op,
                                           "expected element type to be f32");
      }

      // Create a fake window dimension.
      auto fake_window_dims = rewriter.create<linalg::InitTensorOp>(
          loc, fakeWindowShapes, resultType.getElementType());

      SmallVector<Value> resultDynamicDims;
      for (auto& en : llvm::enumerate(resultType.getShape())) {
        if (en.value() != ShapedType::kDynamicSize) continue;
        Value dimSize = rewriter.create<tensor::DimOp>(loc, input, en.index());
        if (en.index() == 0 || en.index() == rank - 1) {
          // batch dims and channel dims can be derived from input dims
          // directly.
          resultDynamicDims.push_back(dimSize);
        } else {
          auto i = en.index() - 1;
          auto stride =
              strides.cast<DenseIntElementsAttr>().getValues<int64_t>()[i];
          auto dilation =
              dilations.cast<DenseIntElementsAttr>().getValues<int64_t>()[i];
          // let j = i * stride
          // output[i] = reduce( input[j, j + window_size * dilation) )
          Value offset = rewriter.create<arith::ConstantIndexOp>(
              loc, fakeWindowShapes[i] * dilation);
          dimSize = rewriter.create<arith::SubIOp>(loc, dimSize, offset);
          dimSize = rewriter.create<arith::DivUIOp>(
              loc, dimSize,
              rewriter.create<arith::ConstantIndexOp>(loc, stride));
          dimSize = rewriter.create<arith::AddIOp>(
              loc, dimSize, rewriter.create<arith::ConstantIndexOp>(loc, 1));
          resultDynamicDims.push_back(dimSize);
        }
      }
      Value initTensor = rewriter.create<linalg::InitTensorOp>(
          loc, resultDynamicDims, resultType.getShape(),
          resultType.getElementType());

      initValue = rewriter.create<tensor::ExtractOp>(loc, initValue);
      Value filled_init_tensor =
          rewriter.create<linalg::FillOp>(loc, initValue, initTensor)
              .getResult(0);
      auto createOp = [&](auto* typePtr) -> linalg::LinalgOp {
        return cast<linalg::LinalgOp>(
            rewriter
                .create<std::remove_pointer_t<decltype(typePtr)>>(
                    loc, ArrayRef<Type>{resultType},
                    ValueRange{input, fake_window_dims.getResult()},
                    filled_init_tensor, strides, dilations,
                    pruneAttributeList(op))
                .getOperation());
      };
      linalg::LinalgOp poolingOp;
      PoolingType poolingType = getPoolingType(op, result.getResultNumber());
      switch (poolingType) {
        case PoolingType::k2DMin: {
          poolingOp = createOp(static_cast<linalg::PoolingNhwcMinOp*>(nullptr));
          break;
        }
        case PoolingType::k3DMin: {
          poolingOp =
              createOp(static_cast<linalg::PoolingNdhwcMinOp*>(nullptr));
          break;
        }
        case PoolingType::k2DMax: {
          poolingOp = createOp(static_cast<linalg::PoolingNhwcMaxOp*>(nullptr));
          break;
        }
        case PoolingType::k3DMax: {
          poolingOp =
              createOp(static_cast<linalg::PoolingNdhwcMaxOp*>(nullptr));
          break;
        }
        case PoolingType::k2DAdd: {
          poolingOp = createOp(static_cast<linalg::PoolingNhwcSumOp*>(nullptr));
          break;
        }
        case PoolingType::k3DAdd: {
          poolingOp =
              createOp(static_cast<linalg::PoolingNdhwcSumOp*>(nullptr));
          break;
        }
        case PoolingType::kInvalid:
          return rewriter.notifyMatchFailure(op, "unknown reduction operation");
      }
      poolingOps.push_back(poolingOp->getResult(0));
    }
    rewriter.replaceOp(op, poolingOps);
    return success();
  }
};

/// Converts xla-hlo.torch_index_select op to a linalg.generic op.
struct TorchIndexSelectOpConversion
    : public OpConversionPattern<mhlo::TorchIndexSelectOp> {
  using OpConversionPattern<mhlo::TorchIndexSelectOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::TorchIndexSelectOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    int axis = static_cast<int>(op.dim());
    int batch = static_cast<int>(op.batch_dims());
    auto indexShapedType = adaptor.index().getType().cast<ShapedType>();
    int numIndices = static_cast<int>(indexShapedType.getRank());
    auto operandShapedType = adaptor.operand().getType().cast<ShapedType>();
    if (axis < 0) axis += static_cast<int>(operandShapedType.getRank());
    if (batch < 0) batch += numIndices;

    Location loc = op.getLoc();
    auto resultType = this->typeConverter->convertType(op.getResult().getType())
                          .cast<ShapedType>();
    int rank = static_cast<int>(resultType.getRank());

    // The output shape is
    //   `params[:axis] + indices[batch_dims:] + params[axis + 1:]`
    SmallVector<Value, 4> dynSizes;
    for (int i = 0; i < rank; ++i) {
      if (!resultType.isDynamicDim(i)) continue;
      if (i < axis) {
        dynSizes.push_back(
            rewriter.create<tensor::DimOp>(loc, adaptor.operand(), i));
      } else if (i < (axis + numIndices - batch)) {
        int idx = i - axis + batch;
        dynSizes.push_back(
            rewriter.create<tensor::DimOp>(loc, adaptor.index(), idx));
      } else {
        int idx = i - (axis + numIndices - batch) + axis + 1;
        dynSizes.push_back(
            rewriter.create<tensor::DimOp>(loc, adaptor.operand(), idx));
      }
    }

    // Generate dummy tensor to preserve slice shape information.
    SmallVector<int64_t> sliceShape;
    SmallVector<Value, 4> dynSliceSizes;
    SmallVector<AffineExpr, 4> sliceExprs;
    auto resultShape = resultType.getShape();
    for (int i = 0; i < axis; ++i) {
      sliceExprs.push_back(rewriter.getAffineDimExpr(i));
      sliceShape.push_back(resultShape[i]);
      if (!resultType.isDynamicDim(i)) continue;
      dynSliceSizes.push_back(
          rewriter.create<tensor::DimOp>(loc, adaptor.operand(), i));
    }
    for (int i = axis + numIndices - batch; i < rank; ++i) {
      sliceExprs.push_back(rewriter.getAffineDimExpr(i));
      sliceShape.push_back(resultShape[i]);
      if (!resultType.isDynamicDim(i)) continue;
      int idx = i - (axis + numIndices - batch) + axis + 1;
      dynSliceSizes.push_back(
          rewriter.create<tensor::DimOp>(loc, adaptor.operand(), idx));
    }

    // Setup AffineMap for operand tensor.
    SmallVector<AffineExpr, 4> exprs;
    for (int i = 0; i < batch; ++i) {
      exprs.push_back(rewriter.getAffineDimExpr(i));
    }
    for (int i = 0, e = numIndices - batch; i < e; ++i) {
      exprs.push_back(rewriter.getAffineDimExpr(axis + i));
    }

    SmallVector<AffineMap, 2> indexingMaps;
    indexingMaps.emplace_back(
        AffineMap::get(rank, /*symbolCount=*/0, exprs, rewriter.getContext()));
    indexingMaps.emplace_back(AffineMap::get(
        rank, /*symbolCount=*/0, sliceExprs, rewriter.getContext()));
    indexingMaps.emplace_back(rewriter.getMultiDimIdentityMap(rank));

    Value sliceOp = rewriter.create<linalg::InitTensorOp>(
        loc, dynSliceSizes, sliceShape, resultType.getElementType());

    Value initOp = rewriter.create<linalg::InitTensorOp>(
        loc, dynSizes, resultType.getShape(), resultType.getElementType());
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, /*resultTensors=*/ArrayRef<Type>{resultType},
        /*inputs=*/ValueRange{adaptor.index(), sliceOp},
        /*outputs=*/initOp, indexingMaps, getNParallelLoopsAttrs(rank),
        /*bodyBuild=*/nullptr, pruneAttributeList(op));

    SmallVector<Type, 4> bodyArgTypes;
    SmallVector<Value, 2> linalgOpArgs = {adaptor.index(), sliceOp};
    // Add a block to the region.
    auto* region = &linalgOp.region();
    auto* block = rewriter.createBlock(region, region->end());
    for (auto blockArgs : linalgOpArgs) {
      bodyArgTypes.push_back(
          blockArgs.getType().cast<ShapedType>().getElementType());
    }
    block->addArguments(bodyArgTypes,
                        SmallVector<Location>(bodyArgTypes.size(), loc));
    block->addArguments(resultType.getElementType(), loc);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToEnd(block);

    Value castedValue = rewriter.create<arith::IndexCastOp>(
        loc, rewriter.getIndexType(), block->getArgument(0));

    SmallVector<Value, 4> indices;
    for (int i = 0; i < axis; ++i) {
      indices.push_back(rewriter.create<linalg::IndexOp>(loc, i));
    }
    indices.push_back(castedValue);
    for (int i = axis + numIndices - batch; i < rank; ++i) {
      indices.push_back(rewriter.create<linalg::IndexOp>(loc, i));
    }
    Value res =
        rewriter.create<tensor::ExtractOp>(loc, adaptor.operand(), indices);
    rewriter.create<linalg::YieldOp>(loc, res);

    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }
};

/// This lowering encompasses the full range of the Gather operation and
/// therefore is very general and just loops over the output and calculate the
/// corresponding input index. It follows the explanation at
/// https://www.tensorflow.org/xla/operation_semantics#gather. The compiler
/// should be able to optimize that a bit, but in order to get efficient
/// lowerings, special-cases of gather should be extracted in separate
/// lowerings, and ideally encapsulated as separate ops or canonicalization
/// patterns.
struct GatherConversion : public OpConversionPattern<mhlo::GatherOp> {
  using OpConversionPattern<mhlo::GatherOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::GatherOp gatherOp, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    Location loc = gatherOp.getLoc();

    Value startIndices = adaptor.start_indices();
    Value operand = adaptor.operand();

    auto resultType = typeConverter->convertType(gatherOp.getType())
                          .dyn_cast<RankedTensorType>();
    RankedTensorType startIndicesType =
        startIndices.getType().dyn_cast<RankedTensorType>();
    // We could actually deal with an unranked result by inferring the result
    // rank, but the current reifyReturnTypes doesn't support unranked either.
    if (!resultType || !startIndicesType)
      return rewriter.notifyMatchFailure(gatherOp,
                                         "unranked start indices or result");

    int resultRank = resultType.getRank();
    // slice_sizes has to have the same size as operand.rank, and doing it this
    // way permits an unranked operand.
    int operandRank = gatherOp.slice_sizes().getNumElements();

    int64_t indexVectorDim = gatherOp.dimension_numbers().getIndexVectorDim();

    ArrayRef<int64_t> offsetDims = gatherOp.dimension_numbers().getOffsetDims();
    ArrayRef<int64_t> collapsedSliceDims =
        gatherOp.dimension_numbers().getCollapsedSliceDims();
    ArrayRef<int64_t> startIndexMap =
        gatherOp.dimension_numbers().getStartIndexMap();

    auto extractAsIndex = [&](Value input, ArrayRef<Value> index) -> Value {
      return rewriter.create<arith::IndexCastOp>(
          loc, rewriter.getIndexType(),
          rewriter.create<tensor::ExtractOp>(loc, input, index));
    };

    // We'll need these later and creating them on demand we end up with
    // duplicates, which also makes lit tests really hard to write.
    SmallVector<Value> constants;
    for (unsigned i = 0; i < std::max(resultRank, operandRank); ++i)
      constants.push_back(
          rewriter.create<arith::ConstantOp>(loc, rewriter.getIndexAttr(i)));

    // Create ops to calculate the dynamic dimensions of the return shape, which
    // are needed for the init tensor.
    SmallVector<Value> dynDimSizes;
    if (!resultType.hasStaticShape()) {
      SmallVector<Value> returnShapes;
      if (failed(gatherOp.reifyReturnTypeShapes(rewriter, adaptor.getOperands(),
                                                returnShapes)))
        return rewriter.notifyMatchFailure(gatherOp,
                                           "could not reify return shape");
      assert(returnShapes.size() == 1);
      Value returnShape = returnShapes[0];

      for (int i = 0; i < resultRank; ++i)
        if (resultType.isDynamicDim(i))
          dynDimSizes.push_back(extractAsIndex(returnShape, constants[i]));
    }

    Value initOp = rewriter.create<linalg::InitTensorOp>(
        loc, dynDimSizes, resultType.getShape(), resultType.getElementType());

    ValueRange ins;
    SmallVector<AffineMap, 1> indexingMaps(
        {rewriter.getMultiDimIdentityMap(resultRank)});
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, /*resultTensorTypes=*/resultType,
        /*inputs=*/ins,
        /*outputs=*/initOp, indexingMaps, getNParallelLoopsAttrs(resultRank),
        /*bodyBuild=*/nullptr, pruneAttributeList(gatherOp));

    // Now populate the linalg generic region
    auto* region = &linalgOp.region();
    auto* block = rewriter.createBlock(region, region->end());
    block->addArguments(resultType.getElementType(), loc);
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToEnd(block);

    // Dimensions in the result that aren't offset dimensions are called batch.
    SmallVector<int64_t> batchDims;
    for (int dim = 0; dim < resultRank; ++dim)
      if (!llvm::is_contained(offsetDims, dim)) batchDims.push_back(dim);

    // Same as with the constants. Creating these all up front is easier than
    // potentially getting duplicates later.
    SmallVector<Value> linalgIndices;
    for (unsigned i = 0; i < resultRank; ++i)
      linalgIndices.push_back(rewriter.create<linalg::IndexOp>(loc, i));

    // Now the complicated part. For a given output dimension we build up an
    // index into the input. It's composed of two parts: the index coming from
    // start_indices, and the offset from that index along the offset
    // dimensions. Everything includes dimension shuffling and remapping as well
    // because of the way gather is defined to allow for any-layout input by
    // adding more attributes.

    // The base gather index (`G` in the documentation) points to a place in
    // start_indices along the batch dimensions.
    SmallVector<Value> gatherIndex;
    for (auto dim : batchDims) gatherIndex.push_back(linalgIndices[dim]);

    SmallVector<Value> indexFromStartIndices;
    for (unsigned i = 0; i < startIndexMap.size(); ++i) {
      // The index along the index_vector dimension of start_indices varies.
      // Basically indexFromStartIndices indexes into a "row" along
      // index_vector_dim, where the row is selected by the current output
      // index.
      // But if index_vector_dim is equal to start_indices.rank, then
      // start_indices gets a trailing 1 dimension added. So the row we're
      // extracting always has length 1 and the index into it is always 0, so we
      // just use the gather index directly
      SmallVector<Value> gCombine(gatherIndex);
      if (indexVectorDim != startIndicesType.getRank()) {
        assert(indexVectorDim <= gCombine.size());
        gCombine.insert(gCombine.begin() + indexVectorDim, constants[i]);
      }

      indexFromStartIndices.push_back(extractAsIndex(startIndices, gCombine));
    }

    // But then start indices are shuffled by the start index map. To make a
    // full index into the operand, all missing indices are zeroes.
    SmallVector<Value> remappedIndexFromIndices(operandRank, constants[0]);
    for (auto& it : llvm::enumerate(startIndexMap))
      remappedIndexFromIndices[it.value()] = indexFromStartIndices[it.index()];

    // Now we construct the index based on the offset. First we need to remap
    // the offset dimensions by dropping the collapsed indices.
    SmallVector<unsigned> remappedOffsetDims;
    for (unsigned i = 0; i < operandRank; ++i)
      if (!llvm::is_contained(collapsedSliceDims, i))
        remappedOffsetDims.push_back(i);

    assert(remappedOffsetDims.size() == offsetDims.size());

    // For the (remapped) offset dimensions, the index is the current index in
    // the output. As before this is expanded to a full index into the operand
    // by using zeroe for the missing indices.
    SmallVector<Value> indexFromOffset(operandRank, constants[0]);
    for (unsigned k = 0; k < offsetDims.size(); ++k)
      indexFromOffset[remappedOffsetDims[k]] = linalgIndices[offsetDims[k]];

    // Now we add together our two indices to get the final index into the
    // operand.
    SmallVector<Value> combinedIndex;
    for (unsigned i = 0; i < operandRank; ++i)
      combinedIndex.push_back(rewriter.create<arith::AddIOp>(
          loc, rewriter.getIndexType(), remappedIndexFromIndices[i],
          indexFromOffset[i]));

    Value element =
        rewriter.create<tensor::ExtractOp>(loc, operand, combinedIndex);
    rewriter.create<linalg::YieldOp>(loc, element);

    rewriter.replaceOp(gatherOp, linalgOp.getResults());

    return success();
  }
};

struct ScatterUpdateConversion : public OpConversionPattern<mhlo::ScatterOp> {
  using OpConversionPattern<mhlo::ScatterOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      mhlo::ScatterOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    // Variadic Scatter support not yet implemented
    if (op.operands().size() != 1 || op.updates().size() != 1) return failure();

    // Check if it is a tensor_scatter_nd_update-like op.
    auto& bodyOps = op.getRegion().front().getOperations();
    if (bodyOps.size() != 1) return failure();
    auto retArg = bodyOps.front().getOperand(0).dyn_cast<BlockArgument>();
    if (!retArg || retArg.getArgNumber() != 1) return failure();

    auto operandTy =
        adaptor.operands()[0].getType().dyn_cast<RankedTensorType>();
    auto indicesTy =
        adaptor.scatter_indices().getType().dyn_cast<RankedTensorType>();
    if (!operandTy || !indicesTy) return failure();

    // Linalg operations put all the computation to the innermost loop. Since we
    // also iterate over scatter_indices() with some loops, we can only check
    // one scatter index in one iteration. If there are multiple indices (ie,
    // the index depth is greater than 1), we don't have a way to keep the
    // comparison state. E.g., if the index_depth is 2, like indices = [[0, 1]],
    // we should use the update value only if (i == 0 and j == 1). However, we
    // can not get both indices in one iteration unless we pack them together.
    auto indexVectorDim = op.scatter_dimension_numbers().getIndexVectorDim();
    if (indicesTy.getDimSize(indexVectorDim) != 1)
      return rewriter.notifyMatchFailure(op, "require index depth to be 1");
    if (indexVectorDim != indicesTy.getRank() - 1) {
      return rewriter.notifyMatchFailure(
          op, "require index_vector_dim to be the last dim");
    }

    // One of indices dims is index depth vector.
    int64_t nloops = operandTy.getRank() + indicesTy.getRank() - 1;
    SmallVector<AffineMap, 3> indexingMaps;
    {
      SmallVector<AffineExpr> exprs;
      for (int64_t i = 0, e = operandTy.getRank(); i < e; ++i)
        exprs.push_back(rewriter.getAffineDimExpr(i));
      indexingMaps.push_back(AffineMap::get(nloops, /*symbolCount=*/0, exprs,
                                            rewriter.getContext()));
    }
    {
      SmallVector<AffineExpr> exprs;
      for (int64_t i = operandTy.getRank(); i < nloops; ++i)
        exprs.push_back(rewriter.getAffineDimExpr(i));
      // The index depth is 1.
      exprs.push_back(rewriter.getAffineConstantExpr(0));
      indexingMaps.push_back(AffineMap::get(nloops, /*symbolCount=*/0, exprs,
                                            rewriter.getContext()));

      exprs.pop_back();
      auto updateWindowDims =
          op.scatter_dimension_numbers().getUpdateWindowDims();
      for (auto d : updateWindowDims)
        exprs.push_back(rewriter.getAffineDimExpr(d));
      indexingMaps.push_back(AffineMap::get(nloops, /*symbolCount=*/0, exprs,
                                            rewriter.getContext()));
    }
    indexingMaps.push_back(indexingMaps.front());

    auto resultTy =
        this->typeConverter->convertType(op.getResults()[0].getType())
            .cast<ShapedType>();
    auto scatterDimsToOperandDims =
        op.scatter_dimension_numbers().getScatterDimsToOperandDims();
    assert(scatterDimsToOperandDims.size() == 1);
    // Do not need init_tensor because we'd like to initialize the output as
    // operand.
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        op.getLoc(), /*resultTensors=*/ArrayRef<Type>{resultTy},
        /*inputs=*/
        ValueRange{adaptor.operands()[0], adaptor.scatter_indices(),
                   adaptor.updates()[0]},
        /*outputs=*/adaptor.operands()[0], indexingMaps,
        getNParallelLoopsAttrs(nloops),
        [scatterDimsToOperandDims](OpBuilder& b, Location loc,
                                   ValueRange args) {
          Value cmpIdx =
              b.create<linalg::IndexOp>(loc, scatterDimsToOperandDims[0]);
          Value idx =
              b.create<arith::IndexCastOp>(loc, b.getIndexType(), args[1]);
          Value pred = b.create<arith::CmpIOp>(
              loc, b.getI1Type(), arith::CmpIPredicate::eq, cmpIdx, idx);
          // Use the output arg, so some update values won't be init value
          // again.
          Value res = b.create<arith::SelectOp>(loc, args[2].getType(), pred,
                                                args[2], args[3]);
          b.create<linalg::YieldOp>(loc, res);
        },
        pruneAttributeList(op));
    rewriter.replaceOp(op, linalgOp.getResults());
    return success();
  }
};

class DotGeneralOpConversion : public OpConversionPattern<mhlo::DotGeneralOp> {
 public:
  using OpConversionPattern<mhlo::DotGeneralOp>::OpConversionPattern;
  LogicalResult matchAndRewrite(
      mhlo::DotGeneralOp op, OpAdaptor adaptor,
      ConversionPatternRewriter& rewriter) const final {
    if (!verifyHloOpBufferOrTensorSemantics(op)) {
      return failure();
    }

    // Get various dimension iterator information
    mhlo::DotDimensionNumbersAttr dimNumbers = op.dot_dimension_numbers();
    auto lhsBatchingDims = dimNumbers.getLhsBatchingDimensions();
    auto rhsBatchingDims = dimNumbers.getRhsBatchingDimensions();
    auto lhsContractingDims = dimNumbers.getLhsContractingDimensions();
    auto rhsContractingDims = dimNumbers.getRhsContractingDimensions();

    // Get shape information and initialize output
    assert(lhsContractingDims.size() == rhsContractingDims.size() &&
           "number of contracting dims must be equal");
    auto numContracting = lhsContractingDims.size();
    // Convert unsigned to signed. This works because signed and unsigned
    // integer matmul is the same operation in two's complement.
    auto outputType =
        typeConverter->convertType(op.getType()).cast<ShapedType>();
    auto target_rank = outputType.getRank();
    auto totalLoopCount = numContracting + target_rank;

    auto lhsRank = adaptor.lhs().getType().cast<ShapedType>().getRank();
    auto lhsExtraDims =
        lhsRank - lhsBatchingDims.size() - lhsContractingDims.size();
    auto rhsRank = adaptor.rhs().getType().cast<ShapedType>().getRank();

    Location loc = op.getLoc();
    auto initTensor =
        getInitTensorFor(rewriter, loc, outputType, op, adaptor.getOperands());
    Value zeroTensor = fillTensorWithZeros(rewriter, loc, initTensor);
    SmallVector<AffineMap, 3> indexing_maps;

    auto getMap = [&](int64_t rank, ArrayRef<int64_t> batchingDims,
                      ArrayRef<int64_t> contractingDims, size_t extraDims) {
      llvm::SmallVector<AffineExpr> indices(rank);
      for (const auto& i : llvm::enumerate(batchingDims)) {
        indices[i.value()] = rewriter.getAffineDimExpr(i.index());
      }
      for (const auto& i : llvm::enumerate(contractingDims)) {
        indices[i.value()] = rewriter.getAffineDimExpr(i.index() + target_rank);
      }
      for (int i = 0; i < rank; ++i) {
        if (!indices[i]) {
          indices[i] = rewriter.getAffineDimExpr(extraDims++);
        }
      }
      indexing_maps.push_back(AffineMap::get(/*dimCount=*/totalLoopCount,
                                             /*symbolCount=*/0, indices,
                                             op->getContext()));
    };
    getMap(lhsRank, lhsBatchingDims, lhsContractingDims,
           lhsBatchingDims.size());
    getMap(rhsRank, rhsBatchingDims, rhsContractingDims,
           rhsBatchingDims.size() + lhsExtraDims);

    {
      SmallVector<AffineExpr, 4> dimExprs;
      dimExprs.reserve(target_rank);
      for (unsigned i = 0; i < target_rank; ++i)
        dimExprs.push_back(rewriter.getAffineDimExpr(i));
      indexing_maps.push_back(AffineMap::get(/*dimCount=*/totalLoopCount,
                                             /*symbolCount=*/0, dimExprs,
                                             op.getContext()));
    }

    Operation* linalgOp = rewriter.create<linalg::GenericOp>(
        loc, /*resultTensorTypes=*/TypeRange{outputType},
        /*inputs=*/ValueRange{adaptor.lhs(), adaptor.rhs()},
        /*outputBuffers=*/ValueRange{zeroTensor}, indexing_maps,
        getParallelAndReductionIterators(
            /*nLoops=*/totalLoopCount,
            /*nReduction=*/numContracting),
        [](OpBuilder& b, Location loc, ValueRange) {
          ImplicitLocOpBuilder builder(loc, b);
          linalg::MatmulOp::regionBuilder(builder, *b.getInsertionBlock(), {});
        },
        pruneAttributeList(op));

    rewriter.replaceOp(op, linalgOp->getResults());
    return success();
  }
};

struct HloLegalizeToLinalgPass
    : public mhlo::HloLegalizeToLinalgPassBase<HloLegalizeToLinalgPass> {
  void getDependentDialects(DialectRegistry& registry) const override {
    registry.insert<bufferization::BufferizationDialect, linalg::LinalgDialect,
                    scf::SCFDialect, complex::ComplexDialect, math::MathDialect,
                    memref::MemRefDialect, shape::ShapeDialect>();
  }

  void runOnOperation() override {
    MLIRContext& ctx = getContext();
    RewritePatternSet patterns(&ctx);
    ConversionTarget target(ctx);
    target.addLegalDialect<
        bufferization::BufferizationDialect, arith::ArithmeticDialect,
        complex::ComplexDialect, linalg::LinalgDialect, math::MathDialect,
        tensor::TensorDialect, sparse_tensor::SparseTensorDialect,
        scf::SCFDialect, shape::ShapeDialect>();

    target.addLegalOp<UnrealizedConversionCastOp>();

    mhlo::RemoveSignTypeConverter typeConverter;
    auto func = getOperation();
    mhlo::populateHLOToLinalgConversionPattern(&ctx, typeConverter, &patterns);
    if (failed(applyPartialConversion(func, target, std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

}  // namespace

namespace mhlo {

void populateHLOToLinalgConversionPattern(MLIRContext* context,
                                          TypeConverter& typeConverter,
                                          RewritePatternSet* patterns) {
  // clang-format off
  patterns->add<
      BroadcastConverter<mhlo::BroadcastOp>, ConcatenateConverter,
      ConstConverterTensor, HloDynamicBroadcastInDimConverter,
      HloBroadcastInDimConverter, IotaConverter<mhlo::IotaOp>,
      EinsumToLinalgConverter,
      IotaConverter<mhlo::DynamicIotaOp>,
      MapOpConverter,
      PointwiseToLinalgConverter<mhlo::AbsOp>,
      PointwiseToLinalgConverter<mhlo::AddOp>,
      PointwiseToLinalgConverter<mhlo::AndOp>,
      PointwiseToLinalgConverter<mhlo::Atan2Op>,
      PointwiseToLinalgConverter<mhlo::BitcastConvertOp>,
      PointwiseToLinalgConverter<mhlo::CbrtOp>,
      PointwiseToLinalgConverter<mhlo::CeilOp>,
      PointwiseToLinalgConverter<mhlo::ClampOp>,
      PointwiseToLinalgConverter<mhlo::ClzOp>,
      PointwiseToLinalgConverter<mhlo::CompareOp>,
      PointwiseToLinalgConverter<mhlo::ComplexOp>,
      PointwiseToLinalgConverter<mhlo::ConvertOp>,
      PointwiseToLinalgConverter<mhlo::CopyOp>,
      PointwiseToLinalgConverter<mhlo::CosOp>,
      PointwiseToLinalgConverter<mhlo::DivOp>,
      PointwiseToLinalgConverter<mhlo::ExpOp>,
      PointwiseToLinalgConverter<mhlo::Expm1Op>,
      PointwiseToLinalgConverter<mhlo::FloorOp>,
      PointwiseToLinalgConverter<mhlo::ImagOp>,
      PointwiseToLinalgConverter<mhlo::IsFiniteOp>,
      PointwiseToLinalgConverter<mhlo::LogOp>,
      PointwiseToLinalgConverter<mhlo::LogisticOp>,
      PointwiseToLinalgConverter<mhlo::Log1pOp>,
      PointwiseToLinalgConverter<mhlo::MaxOp>,
      PointwiseToLinalgConverter<mhlo::MinOp>,
      PointwiseToLinalgConverter<mhlo::MulOp>,
      PointwiseToLinalgConverter<mhlo::NegOp>,
      PointwiseToLinalgConverter<mhlo::NotOp>,
      PointwiseToLinalgConverter<mhlo::OrOp>,
      PointwiseToLinalgConverter<mhlo::PopulationCountOp>,
      PointwiseToLinalgConverter<mhlo::PowOp>,
      PointwiseToLinalgConverter<mhlo::RealOp>,
      PointwiseToLinalgConverter<mhlo::RemOp>,
      PointwiseToLinalgConverter<mhlo::RoundOp>,
      PointwiseToLinalgConverter<mhlo::RsqrtOp>,
      PointwiseToLinalgConverter<mhlo::SelectOp>,
      PointwiseToLinalgConverter<mhlo::ShiftLeftOp>,
      PointwiseToLinalgConverter<mhlo::ShiftRightArithmeticOp>,
      PointwiseToLinalgConverter<mhlo::ShiftRightLogicalOp>,
      PointwiseToLinalgConverter<mhlo::SignOp>,
      PointwiseToLinalgConverter<mhlo::SinOp>,
      PointwiseToLinalgConverter<mhlo::SqrtOp>,
      PointwiseToLinalgConverter<mhlo::SubOp>,
      PointwiseToLinalgConverter<mhlo::TanhOp>,
      PointwiseToLinalgConverter<mhlo::XorOp>,
      RealDynamicSliceConverter,
      ReshapeOpConverter,
      ReverseConverter,
      SliceConverter,
      DynamicSliceConverter,
      DynamicUpdateSliceConverter,
      TransposeConverter<mhlo::TransposeOp>,
      NormalConvOpConversion,
      DepthwiseConvOpConversion,
      GatherConversion,
      PadOpConversion,
      PadOpNegativePaddingConversion,
      ReduceConversion,
      ReduceWindowOpOnTensorsGenericConversion,
      ReduceWindowOpConversion,
      RngUniformConversion,
      ScatterUpdateConversion,
      TorchIndexSelectOpConversion>(typeConverter, context);
    patterns->add<
      DotOpConversion<DotOperationType::kMatrixMatrix, linalg::MatmulOp>,
      DotOpConversion<DotOperationType::kMatrixVector, linalg::MatvecOp>,
      DotOpConversion<DotOperationType::kVectorMatrix, linalg::VecmatOp>,
      DotOpConversion<DotOperationType::kVectorDot, linalg::DotOp>,
      DotGeneralBatchMatMulOpConversion>(typeConverter, context,
                                         PatternBenefit(2));
  // clang-format on
  patterns->add<DotGeneralOpConversion>(typeConverter, context,
                                        PatternBenefit(1));
  patterns->add<ReduceRegionXLAOpConversion<mhlo::AddOp>,
                ReduceRegionXLAOpConversion<mhlo::AndOp>,
                ReduceRegionXLAOpConversion<mhlo::CompareOp>,
                ReduceRegionXLAOpConversion<mhlo::ImagOp>,
                ReduceRegionXLAOpConversion<mhlo::MaxOp>,
                ReduceRegionXLAOpConversion<mhlo::MinOp>,
                ReduceRegionXLAOpConversion<mhlo::MulOp>,
                ReduceRegionXLAOpConversion<mhlo::OrOp>,
                ReduceRegionXLAOpConversion<mhlo::RealOp>,
                ReduceRegionXLAOpConversion<mhlo::SelectOp>,
                ReduceRegionReturnOpConversion>(context, PatternBenefit(1000));
}

std::unique_ptr<OperationPass<func::FuncOp>> createLegalizeHloToLinalgPass() {
  return std::make_unique<HloLegalizeToLinalgPass>();
}

std::unique_ptr<TypeConverter> createHloToLinalgSignedIntegerConverter() {
  return std::make_unique<RemoveSignTypeConverter>();
}

}  // namespace mhlo
}  // namespace mlir
