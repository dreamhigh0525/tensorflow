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

#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_a_m.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <tuple>
#include <type_traits>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/Dialect/Traits.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Diagnostics.h"  // from @llvm-project
#include "mlir/IR/DialectImplementation.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/Identifier.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/OpImplementation.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Parser.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/InliningUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_attributes.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_side_effects.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_structs.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/util/tensor_format.h"

namespace mlir {
namespace TF {

namespace {
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_helpers.inc"
#include "tensorflow/compiler/mlir/tensorflow/transforms/generated_canonicalize.inc"
}  // namespace

//===----------------------------------------------------------------------===//
// AddOp
//===----------------------------------------------------------------------===//

void AddOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  results.insert<AddToAddV2>(context);
}

//===----------------------------------------------------------------------===//
// AddNOp
//===----------------------------------------------------------------------===//

OpFoldResult AddNOp::fold(ArrayRef<Attribute> operands) {
  if (operands.size() == 1) return *inputs().begin();
  return {};
}

//===----------------------------------------------------------------------===//
// AddV2Op
//===----------------------------------------------------------------------===//

void AddV2Op::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context) {
  results.insert<AddV2OfNegLeft, AddV2OfNegRight>(context);
}

OpFoldResult AddV2Op::fold(ArrayRef<Attribute> operands) {
  return IdentityArithmeticOpFolder<AddV2Op>(*this, operands);
}

//===----------------------------------------------------------------------===//
// AllOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(AllOp op) {
  return VerifyReductionInputAndDims(op.input(), op.reduction_indices(),
                                     op.getLoc());
}

//===----------------------------------------------------------------------===//
// AnyOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(AnyOp op) {
  return VerifyReductionInputAndDims(op.input(), op.reduction_indices(),
                                     op.getLoc());
}

//===----------------------------------------------------------------------===//
// AssertOp
//===----------------------------------------------------------------------===//

namespace {

// Removes Assert with constant true predicate.
struct AssertWithTrue : public OpRewritePattern<AssertOp> {
  using OpRewritePattern<AssertOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssertOp op,
                                PatternRewriter &rewriter) const override {
    ElementsAttr cst;
    if (matchPattern(op.condition(), m_Constant(&cst))) {
      if (cst.getValue<BoolAttr>({}).getValue()) {
        rewriter.eraseOp(op);
        return success();
      }
    }
    return failure();
  }
};
}  // namespace

void AssertOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                           MLIRContext *context) {
  results.insert<AssertWithTrue>(context);
}

//===----------------------------------------------------------------------===//
// BatchMatMulOp
//===----------------------------------------------------------------------===//

void BatchMatMulOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<BatchMatMulToMatMul>(context);
}

//===----------------------------------------------------------------------===//
// BatchMatMulV2Op
//===----------------------------------------------------------------------===//

static LogicalResult Verify(BatchMatMulV2Op op) {
  if (!HasRankAtLeast(op.x(), 2)) {
    return op.emitOpError("requires lhs operand to have rank at least two");
  }
  if (!HasRankAtLeast(op.y(), 2)) {
    return op.emitOpError("requires rhs operand to have rank at least two");
  }
  return success();
}

void BatchMatMulV2Op::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<BatchMatMulV2ToMatMul>(context);
}

//===----------------------------------------------------------------------===//
// BatchToSpaceOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(BatchToSpaceOp op) {
  // Op already has a constraint that block_size >= 2.
  int64_t block_size = op.block_size().getSExtValue();

  llvm::SmallVector<int64_t, 4> input_shape(4, ShapedType::kDynamicSize);
  auto input_type = op.input().getType().cast<TensorType>();
  if (input_type.hasRank()) {
    if (input_type.getRank() != 4)
      return op.emitOpError()
             << "requires input to be a 4D tensor, but got " << input_type;

    int64_t input_batch = input_type.getDimSize(0);
    if (input_batch != ShapedType::kDynamicSize &&
        input_batch % (block_size * block_size) != 0) {
      return op.emitOpError()
             << "requires input batch (dimension 0) to be evenly divisible "
                "by (block_size * block_size), but got input batch "
             << input_batch << " and block_size " << block_size;
    }

    input_shape.assign(input_type.getShape().begin(),
                       input_type.getShape().end());
  }

  auto crops_type = op.crops().getType().cast<TensorType>();
  if (crops_type.hasRank()) {
    if (crops_type.getRank() != 2)
      return op.emitOpError()
             << "requires crops to be a 2D tensor, but got " << crops_type;

    auto dim_of_size = [&](int64_t dim, int64_t size) {
      if (crops_type.isDynamicDim(dim)) return true;
      return crops_type.getDimSize(dim) == size;
    };
    if (!dim_of_size(0, 2) || !dim_of_size(1, 2))
      return op.emitOpError()
             << "requires crops to be a tensor<2x2>, but got " << crops_type;
  }

  DenseIntElementsAttr crops_attr;
  // Crops are defined as [[crop_top, crop_bottom], [crop_left, crop_right]],
  // and flattened as [crop_top, crop_bottom, crop_left, crop_right]
  llvm::SmallVector<int64_t, 4> crops_values;
  if (matchPattern(op.crops(), m_Constant(&crops_attr))) {
    assert(crops_attr.getNumElements() == 4 &&
           "tf.BatchToSpace crops must have 4 elements");

    auto crops_range = crops_attr.getIntValues();
    for (const auto &crops_value : crops_range) {
      int64_t crops_value_int = crops_value.getSExtValue();
      if (crops_value_int < 0)
        return op.emitOpError()
               << "requires all crop values to be nonnegative, but got "
               << crops_attr;

      crops_values.push_back(crops_value_int);
    }
  }

  auto output_type = op.output().getType().cast<TensorType>();
  if (output_type.hasRank()) {
    if (output_type.getRank() != 4)
      return op.emitOpError()
             << "requires output to be a 4D tensor, but got " << output_type;

    auto static_dims = [](int64_t dim_a, int64_t dim_b) {
      return dim_a != ShapedType::kDynamicSize &&
             dim_b != ShapedType::kDynamicSize;
    };

    auto output_shape = output_type.getShape();

    // output batch = input batch / (block_size * block_size).
    int64_t input_batch = input_shape[0];
    int64_t output_batch = output_shape[0];
    if (static_dims(input_batch, output_batch) &&
        (output_batch * block_size * block_size) != input_batch)
      return op.emitOpError()
             << "requires output batch (dimension 0) to be equal to input "
                "batch (dimension 0) / (block_size * block_size), but got "
                "output batch "
             << output_batch << ", input batch " << input_batch
             << ", and block_size " << block_size;

    auto check_spatial_dim = [&](int64_t spatial_dim_index,
                                 llvm::StringRef dim_name,
                                 llvm::StringRef crop_a_name,
                                 llvm::StringRef crop_b_name) -> LogicalResult {
      int64_t input_dim = input_shape[spatial_dim_index];
      int64_t output_dim = output_shape[spatial_dim_index];
      if (!static_dims(input_dim, output_dim)) return success();

      int64_t input_dim_pad = input_dim * block_size;
      // If crops are unknown, the maximum output spatial dim size is input
      // spatial dim size * block_size, as crops can be minimum 0.
      if (crops_values.empty() && output_dim > input_dim * block_size)
        return op.emitOpError()
               << "requires output " << dim_name << " (dimension "
               << spatial_dim_index << ") to be less than or equal to input "
               << dim_name << " (dimension " << spatial_dim_index
               << ") * block_size, but got output " << dim_name << " "
               << output_dim << ", input " << dim_name << " " << input_dim
               << ", and block_size " << block_size;

      if (!crops_values.empty()) {
        // output spatial dim = input spatial dim * block_size - crops.
        int64_t crop_a = crops_values[2 * (spatial_dim_index - 1)];
        int64_t crop_b = crops_values[2 * (spatial_dim_index - 1) + 1];
        if (output_dim != input_dim_pad - crop_a - crop_b)
          return op.emitOpError()
                 << "requires output " << dim_name << " (dimension "
                 << spatial_dim_index << ") to be equal to input " << dim_name
                 << " (dimension " << spatial_dim_index << ") * block_size - "
                 << crop_a_name << " - " << crop_b_name << ", but got output "
                 << dim_name << " " << output_dim << ", input " << dim_name
                 << " " << input_dim << ", " << crop_a_name << " " << crop_a
                 << ", " << crop_b_name << " " << crop_b << ", and block_size "
                 << block_size;
      }

      return success();
    };

    if (failed(check_spatial_dim(1, "height", "crop_top", "crop_bottom")) ||
        failed(check_spatial_dim(2, "width", "crop_left", "crop_right")))
      return failure();

    int64_t input_depth = input_shape[3];
    int64_t output_depth = output_shape[3];
    if (static_dims(input_depth, output_depth) && output_depth != input_depth)
      return op.emitOpError()
             << "requires output depth (dimension 3) to be equal to input "
                "depth (dimension 3), but got output depth "
             << output_depth << " and input depth " << input_depth;
  }

  return success();
}

void BatchToSpaceOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<BatchToSpaceToBatchToSpaceND>(context);
}

//===----------------------------------------------------------------------===//
// BiasAddOp
//===----------------------------------------------------------------------===//

// Verifies that,
// * the value and bias operands have valid ranks or are unranked.
// * Channel dimension of the value operand and length of bias matches if they
//   are not unknown.
//
static LogicalResult Verify(BiasAddOp op) {
  StringRef format = op.data_format();
  if (format == "NHWC") {
    if (!HasRankAtLeast(op.value(), 2))
      return op.emitOpError(
          "requires value operand to have rank at least two with `NHWC` data "
          "format");
  } else {
    // Op definition requires data_format to be either NHWC or NCHW.
    DCHECK_EQ(format.str(), "NCHW");
    if (!HasRankAtLeast(op.value(), 3))
      return op.emitOpError(
          "requires value operand to have rank at least three with `NCHW` data "
          "format");
  }

  if (!IsOfRankOrUnranked(op.bias(), 1))
    return op.emitOpError("requires bias operand to have rank exactly one");

  RankedTensorType value_ty = op.value().getType().dyn_cast<RankedTensorType>();
  RankedTensorType bias_ty = op.bias().getType().dyn_cast<RankedTensorType>();
  if (!bias_ty || !value_ty) return success();

  // TODO(hinsu): Leverage tensor_format.h utility in TensorFlow to compute
  // dimension indices based on format.
  int64_t feature_dim_idx = format == "NHWC" ? value_ty.getRank() - 1 : 1;
  int64_t feature_dim = value_ty.getDimSize(feature_dim_idx);
  int64_t bias_len = bias_ty.getDimSize(0);
  if (feature_dim != -1 && bias_len != -1 && feature_dim != bias_len) {
    return op.emitOpError()
           << "requires channel dimension and feature dimension to match; "
              "found "
           << feature_dim << " and " << bias_len << ", respectively";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// BiasAddGradOp
//===----------------------------------------------------------------------===//

// Verifies that,
// * the out_backprop operands have valid ranks or are unranked.
//
static LogicalResult Verify(BiasAddGradOp op) {
  StringRef format = op.data_format();
  if (format == "NHWC") {
    if (!HasRankAtLeast(op.out_backprop(), 2))
      return op.emitOpError(
          "requires out_backprop operand to have rank at least two with `NHWC` "
          "data format");
  } else {
    // Op definition requires data_format to be either NHWC or NCHW.
    DCHECK_EQ(format.str(), "NCHW");
    if (!HasRankAtLeast(op.out_backprop(), 3))
      return op.emitOpError(
          "requires out_backprop operand to have rank at least three with "
          "`NCHW` data format");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// BiasAddV1Op
//===----------------------------------------------------------------------===//

void BiasAddV1Op::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<BiasAddV1ToBiasAdd>(context);
}

//===----------------------------------------------------------------------===//
// BitcastOp
//===----------------------------------------------------------------------===//

void BitcastOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<BitcastSameType, BitcastNested>(context);
}

//===----------------------------------------------------------------------===//
// BroadcastToOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(BroadcastToOp op) {
  // TODO(antiagainst): check that
  // * The 'shape' input is an 1-D int tensor.
  // * Each dimension pair of the source and target shapes are either equal
  //   or one of them is one.
  return success();
}

//===----------------------------------------------------------------------===//
// CaseOp
//===----------------------------------------------------------------------===//

class FoldConstantCaseOp : public OpRewritePattern<TF::CaseOp> {
 public:
  explicit FoldConstantCaseOp(MLIRContext *context)
      : OpRewritePattern<TF::CaseOp>(context) {}
  LogicalResult matchAndRewrite(TF::CaseOp op,
                                PatternRewriter &rewriter) const override;
};

LogicalResult FoldConstantCaseOp::matchAndRewrite(
    TF::CaseOp op, PatternRewriter &rewriter) const {
  // Extract the constant cond value.
  DenseIntElementsAttr branch;
  if (!matchPattern(op.branch_index(), m_Constant(&branch))) return failure();

  // Only attempt to fold scalar valued case statements.
  // TODO(jpienaar): This can be removed if CaseOp's verifier covers it.
  if (!branch.getType().cast<RankedTensorType>().getShape().empty())
    return failure();

  int index = *branch.getValues<int>().begin();
  // TODO(jpienaar): This can be removed if CaseOp's verifier covers it.
  if (index >= op.branches().size()) return failure();

  auto func = op.branches()[index].cast<SymbolRefAttr>();
  auto empty = rewriter.getStringAttr("");
  auto call_op = rewriter.create<PartitionedCallOp>(
      op.getLoc(), op.getResultTypes(), op.getOperands().drop_front(), func,
      /*config=*/empty, /*config_proto=*/empty, /*executor_type=*/empty);
  PropagateDeviceAndInternalAttrs(op.getOperation(), call_op);
  rewriter.replaceOp(op, call_op.getResults());
  return success();
}

void CaseOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                         MLIRContext *context) {
  results.insert<FoldConstantCaseOp>(context);
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

OpFoldResult CastOp::fold(ArrayRef<Attribute> operands) {
  // Cast with the same type is a no-op.
  Value operand = getOperand();
  if (getType() == operand.getType()) return operand;
  return {};
}

//===----------------------------------------------------------------------===//
// ConcatOp and ConcatV2Op
//===----------------------------------------------------------------------===//

template <typename OpT,
          typename std::enable_if<llvm::is_one_of<
              OpT, ConcatOp, ConcatV2Op>::value>::type * = nullptr>
static LogicalResult Verify(OpT op) {
  // TODO(hinsu): Convert variadic length attributes to derived attributes.
  Operation::operand_range values = op.values();

  int axis_idx = std::is_same<OpT, ConcatOp>() ? 0 : 1;
  Value axis = *op.getODSOperands(axis_idx).begin();
  if (!HasRankAtMost(axis, 1)) {
    return op.emitOpError(
        "requires axis to be of scalar type (or vector type for older "
        "versions)");
  }

  return VerifyTypesCompatibility(values,
                                  /*mask_one_dim=*/true, op.getOperation());
}

void ConcatOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                           MLIRContext *context) {
  results.insert<ConvertToConcatV2>(context);
}

//===----------------------------------------------------------------------===//
// ConcatOffsetOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(ConcatOffsetOp op) {
  if (op.N() < 2)
    return op.emitOpError() << "requires N to be at least 2, got " << op.N();

  if (op.shape().size() != op.offset().size())
    return op.emitOpError()
           << "requires sizes of shapes and offsets to be the same, got sizes "
           << op.shape().size() << " and " << op.offset().size();

  auto ranked_dim = op.concat_dim().getType().dyn_cast<RankedTensorType>();
  if (ranked_dim && ranked_dim.getRank() != 0)
    return op.emitOpError()
           << "requires concat_dim to be a scalar, got tensor of rank "
           << ranked_dim.getRank();

  int64_t num_dims = -1;
  for (auto shape_offset_idx :
       llvm::enumerate(llvm::zip(op.shape(), op.offset()))) {
    Value shape = std::get<0>(shape_offset_idx.value());
    Value offset = std::get<1>(shape_offset_idx.value());
    const size_t idx = shape_offset_idx.index();

    if (failed(verifyCompatibleShape(shape.getType(), offset.getType())))
      return op.emitOpError() << "requires operand and result " << idx
                              << " to have compatible shapes";

    auto ranked_shape = shape.getType().dyn_cast<RankedTensorType>();
    if (!ranked_shape) continue;

    if (ranked_shape.getRank() != 1)
      return op.emitOpError() << "requires shape tensor operand " << idx
                              << " to be of rank 1, got tensor of rank "
                              << ranked_shape.getRank();

    if (!ranked_shape.hasStaticShape()) continue;

    int64_t ranked_shape_dim = ranked_shape.getDimSize(0);
    if (num_dims == -1)
      num_dims = ranked_shape_dim;
    else if (ranked_shape_dim != num_dims)
      return op.emitOpError()
             << "requires shape tensor (rank 1) operand " << idx
             << " to be of length " << num_dims
             << ", got tensor (rank 1) of length " << ranked_shape_dim;
  }

  return success();
}

LogicalResult ConcatOffsetOp::fold(ArrayRef<Attribute> operands,
                                   SmallVectorImpl<OpFoldResult> &results) {
  // ConcatOffset must have its first operand be concat_dim and at least two
  // shape tensors in variadic shapes operand.
  if (operands.size() < 3) return failure();

  // Check concat_dim is a scalar.
  auto concat_dim_attr = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!concat_dim_attr || concat_dim_attr.getType().getRank() != 0)
    return failure();

  llvm::SmallVector<DenseIntElementsAttr, 4> shapes;
  shapes.reserve(operands.size() - 1);
  for (Attribute shape : llvm::drop_begin(operands, 1))
    if (auto shape_attr = shape.dyn_cast_or_null<DenseIntElementsAttr>())
      shapes.push_back(shape_attr);
    else
      return failure();

  // Check all shapes are vectors of the same length.
  if (shapes.front().getType().getRank() != 1) return success();
  const int64_t num_dims = shapes.front().getNumElements();
  for (DenseIntElementsAttr shape : llvm::drop_begin(shapes, 1))
    if (shape.getType().getRank() != 1 || shape.getNumElements() != num_dims)
      return failure();

  // Check concat_dim is within [-num_dims, num_dims).
  int32_t concat_dim = (*concat_dim_attr.getValues<int32_t>().begin());
  if (concat_dim < 0) concat_dim += num_dims;
  if (concat_dim >= num_dims || concat_dim < 0) return failure();

  // Check all elements besides at concat_dim match across all shape tensors.
  SmallVector<int32_t, 4> shape0;
  shape0.reserve(num_dims);
  for (int32_t dim : shapes.front().getValues<int32_t>()) shape0.push_back(dim);

  for (DenseIntElementsAttr shape : llvm::drop_begin(shapes, 1)) {
    for (auto dims_and_idx : llvm::enumerate(llvm::zip(shape0, shape))) {
      if (dims_and_idx.index() == concat_dim) continue;

      if (std::get<0>(dims_and_idx.value()) !=
          std::get<1>(dims_and_idx.value()).getSExtValue())
        return failure();
    }
  }

  // Compute an exclusive cumulative sum of elements at concat_dim.
  results.reserve(shapes.size());
  SmallVector<int32_t, 4> cumulative_sum(num_dims, 0);
  RankedTensorType offset_type =
      RankedTensorType::get({num_dims}, IntegerType::get(32, getContext()));
  for (DenseIntElementsAttr shape : shapes) {
    results.push_back(DenseIntElementsAttr::get(offset_type, cumulative_sum));
    cumulative_sum[concat_dim] += shape.getValue<int32_t>(concat_dim);
  }

  return success();
}

//===----------------------------------------------------------------------===//
// ConjOp
//===----------------------------------------------------------------------===//

void ConjOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                         MLIRContext *context) {
  results.insert<ConjNested>(context);
}

//===----------------------------------------------------------------------===//
// ConstOp
//===----------------------------------------------------------------------===//

OpFoldResult ConstOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.empty() && "constant has no operands");

  // Return the held attribute value.
  return value();
}

// Builds a constant op with the specified attribute `value`. The result
// op's type is deduced from `value`; if `value` is of scalar type,
// wraps it up with a tensor type of empty shape.
// TODO(jpienaar): This one differs from the autogenerated one as it takes an
// attribute but always creates an ElementsAttr internally.
void ConstOp::build(OpBuilder &builder, OperationState &result,
                    Attribute value) {
  ShapedType type;
  if (auto elem_attr = value.dyn_cast<ElementsAttr>()) {
    return ConstOp::build(builder, result, elem_attr);
  } else if (value.isa<BoolAttr, FloatAttr, IntegerAttr>()) {
    // All TensorFlow types must be tensor types. In the build() method,
    // we want to provide more flexibility by allowing attributes of scalar
    // types. But we need to wrap it up with ElementsAttr to construct
    // valid TensorFlow constants.
    type = RankedTensorType::get(/*shape=*/{}, value.getType());
    return ConstOp::build(builder, result, DenseElementsAttr::get(type, value));
  }
  // TODO(jpienaar): support other TensorFlow specific types.
  llvm_unreachable("unsupported attribute type for building tf.Const");
}

void ConstOp::build(OpBuilder &builder, OperationState &result, Type type,
                    Attribute value) {
  // Handle the case where the type and value are already tensors.
  if (type.isa<TensorType>() && value.isa<ElementsAttr>()) {
    result.addTypes(type);
    result.addAttribute("value", value);
    return;
  }

  // Otherwise, default to the attribute builder.
  ConstOp::build(builder, result, value);
  assert(type == result.types[0] && "type mismatch in construction");
}

LogicalResult ConstOp::inferReturnTypes(
    MLIRContext *context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type> &inferredReturnTypes) {
  auto value = attributes.get("value");
  if (!value) return emitOptionalError(location, "missing attribute 'value'");
  if (auto elem_attr = value.dyn_cast<ElementsAttr>()) {
    inferredReturnTypes.assign({elem_attr.getType()});
    return success();
  }
  return emitOptionalError(location,
                           "attribute 'value' failed to satisfy constraint: "
                           "constant vector/tensor");
}

//===----------------------------------------------------------------------===//
// Conv2DOp and Conv3DOp
//===----------------------------------------------------------------------===//

template <typename OpT>
static LogicalResult VerifyConvOpAttributes(OpT op, int num_dims) {
  if (!IsOfRankOrUnranked(op.getResult(), num_dims))
    return op.emitOpError()
           << "requires result to be " << num_dims << "D tensor";

  auto is_not_positive = [](Attribute val) {
    return val.cast<IntegerAttr>().getValue().getSExtValue() <= 0;
  };

  int64_t strides_size = op.strides().size();
  if (strides_size != num_dims)
    return op.emitOpError() << "requires strides attribute length to be "
                            << num_dims << "; actual length " << strides_size;
  if (llvm::any_of(op.strides().getValue(), is_not_positive))
    return op.emitOpError("requires positive strides");

  int64_t dilations_size = op.strides().size();
  if (op.dilations().size() != num_dims)
    return op.emitOpError() << "requires dilations attribute length to be "
                            << num_dims << "; actual length " << dilations_size;
  if (llvm::any_of(op.dilations().getValue(), is_not_positive))
    return op.emitOpError("requires positive dilations");

  return success();
}

// Verifies that,
// * Ranks of operands and result are valid
// * Number of input channels is divisible by the number of filter input
//   channels
// * Length of explicit_paddings attribute is valid and has non negative
//   elements
// * strides and dilations attributes have positive elements
template <typename OpT, typename std::enable_if<llvm::is_one_of<
                            OpT, Conv2DOp, Conv3DOp>::value>::type * = nullptr>
static LogicalResult Verify(OpT op) {
  int num_spatial_dims = std::is_same<OpT, Conv2DOp>() ? 2 : 3;
  int num_dims = 2 + num_spatial_dims;

  if (!IsOfRankOrUnranked(op.input(), num_dims) ||
      !IsOfRankOrUnranked(op.filter(), num_dims))
    return op.emitOpError()
           << "requires operands to be " << num_dims << "D tensor";

  // EXPLICIT padding mode and the associated attribute is limited to Conv2D.
  // So, fetch attribute by string instead of the op.explicit_paddings()
  // attribute getter.
  if (op.padding() == "EXPLICIT") {
    auto paddings = op.template getAttrOfType<ArrayAttr>("explicit_paddings");
    if (!paddings)
      return op.emitOpError() << "requires attribute 'explicit_paddings' with "
                                 "'EXPLICIT' padding mode";

    int64_t paddings_size = paddings.size();
    int64_t expected_size = 2 * num_dims;

    if (paddings_size != expected_size)
      return op.emitOpError()
             << "requires explicit_paddings attribute length to be "
             << expected_size << "; actual length " << paddings_size;

    auto is_negative = [](Attribute val) {
      return val.cast<IntegerAttr>().getValue().getSExtValue() < 0;
    };
    if (llvm::any_of(paddings.getValue(), is_negative))
      return op.emitOpError("requires non negative explicit paddings");
  }

  LogicalResult verify_result = VerifyConvOpAttributes(op, num_dims);
  if (failed(verify_result)) {
    return verify_result;
  }

  int64_t input_channels = -1;
  if (auto ty = op.input().getType().template dyn_cast<RankedTensorType>()) {
    std::string data_format = op.data_format().str();
    tensorflow::TensorFormat format;
    auto is_valid = FormatFromString(data_format, &format);
    DCHECK(is_valid) << data_format;
    int idx = tensorflow::GetTensorFeatureDimIndex(num_dims, format);
    input_channels = ty.getDimSize(idx);
  }

  int64_t filter_channels = -1;
  if (auto ty = op.filter().getType().template dyn_cast<RankedTensorType>()) {
    int idx = tensorflow::GetFilterTensorInputChannelsDimIndex(
        num_dims, tensorflow::FORMAT_HWIO);
    filter_channels = ty.getDimSize(idx);
  }

  if (input_channels != -1 && filter_channels != -1 &&
      input_channels % filter_channels != 0)
    return op.emitOpError()
           << "requires the number of input channels to be divisible by the "
              "number of filter input channels; found "
           << input_channels << " and " << filter_channels << ", respectively";

  return success();
}

LogicalResult Conv2DOp::UpdateDataFormat(StringRef data_format) {
  auto perm = GetDataFormatPermutation(this->data_format(), data_format);
  if (perm.empty()) return failure();

  // Update data_format attribute and result types.
  if (failed(::mlir::TF::UpdateDataFormat(data_format, this))) return failure();

  // Update convolution attributes.
  setAttr("dilations", ShuffleArrayAttr(dilations(), perm));
  setAttr("strides", ShuffleArrayAttr(strides(), perm));
  setAttr("explicit_paddings", ShuffleArrayAttr(explicit_paddings(), perm, 2));

  return success();
}

StringRef Conv2DOp::GetOptimalLayout(const RuntimeDevices &devices) {
  // Keep current data format if no GPUs are available or if explicit placement
  // does not allow to use GPU for this operation.
  if (!CanUseGpuDevice(devices) || !CanUseGpuDevice(getOperation()))
    return data_format();

  // Input must be a tensor.
  auto input_ty = input().getType().dyn_cast<TensorType>();
  if (!input_ty) return data_format();

  // For f16 data type on devices with Tensor Cores support NHWC data format
  // is up to ~2x faster.
  const bool is_f16 = input_ty.getElementType().isF16();
  if (is_f16 && CanUseTensorCores(devices)) return "NHWC";

  // For f32/f16 data type decision depends on the filter size in spatial
  // dimensions, for other data types we keep current data format.
  if (!input_ty.getElementType().isF32() && !input_ty.getElementType().isF16())
    return data_format();

  // Keep current data format if filter rank is unknown or not equal to 4.
  auto filter_ty = filter().getType().dyn_cast<RankedTensorType>();
  if (!filter_ty || filter_ty.getRank() != 4) return data_format();

  const int64_t d0 = filter_ty.getDimSize(0);
  const int64_t d1 = filter_ty.getDimSize(1);

  auto all_ones = [](ArrayAttr arr) -> bool {
    return llvm::all_of(arr, [](Attribute attr) -> bool {
      return attr.cast<IntegerAttr>().getInt() == 1;
    });
  };

  // Convolutions with 1x1 filter and with strides and dilations all ones, can
  // be computed as a GEMM in NHWC data format, and can be up to ~2x times
  // faster than convolution in NCHW.
  const bool one_by_one = d0 == 1 && d1 == 1;
  const bool trivial_strides = all_ones(strides());
  const bool trivial_dilations = all_ones(dilations());

  // TODO(ezhulenev): This might lead to excessive transposes in the final IR,
  // if the ratio of 1x1 convolutions to regular convolutions is close to 1:1.
  // Also FusedBatchNorm in training mode prefers NCHW data format. Check if all
  // users can efficiently use NHWC data format?
  if (one_by_one && trivial_strides && trivial_dilations) {
    return "NHWC";
  }

  // If filter spatial dimensions are unknown or not 1x1 we prefer NCHW, because
  // it's the fastest option on NVIDIA GPUs with cuDNN library support.
  return "NCHW";
}

//===----------------------------------------------------------------------===//
// Conv2dBackpropFilterOp
//===----------------------------------------------------------------------===//

LogicalResult Conv2DBackpropFilterOp::UpdateDataFormat(StringRef data_format) {
  StringRef src_data_format = this->data_format();

  auto perm = GetDataFormatPermutation(src_data_format, data_format);
  if (perm.empty()) return failure();

  // Update data_format attribute and result types.
  if (failed(::mlir::TF::UpdateDataFormat(data_format, this))) return failure();

  // Update convolution attributes.
  setAttr("dilations", ShuffleArrayAttr(dilations(), perm));
  setAttr("strides", ShuffleArrayAttr(strides(), perm));
  setAttr("explicit_paddings", ShuffleArrayAttr(explicit_paddings(), perm, 2));

  // Permute filter sizes operand.
  OpBuilder builder(getOperation());
  auto filter_sizes_permuted = builder.create<TF::DataFormatVecPermuteOp>(
      getLoc(), filter_sizes(), StringAttr::get(src_data_format, getContext()),
      StringAttr::get(data_format, getContext()));
  setOperand(1, filter_sizes_permuted);

  return success();
}

StringRef Conv2DBackpropFilterOp::GetOptimalLayout(
    const RuntimeDevices &devices) {
  // Keep current data format if no GPUs are available or if explicit placement
  // does not allow to use GPU for this operation.
  if (!CanUseGpuDevice(devices) || !CanUseGpuDevice(getOperation()))
    return data_format();

  // Input must be a tensor.
  auto input_ty = input().getType().dyn_cast<TensorType>();
  if (!input_ty) return data_format();

  // For f16 data type on devices with Tensor Cores support NHWC data format
  // is up to ~2x faster.
  const bool is_f16 = input_ty.getElementType().isF16();
  if (is_f16 && CanUseTensorCores(devices)) return "NHWC";

  // Otherwise always use "NCHW".
  return "NCHW";
}

//===----------------------------------------------------------------------===//
// Conv2DBackpropInputOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(Conv2DBackpropInputOp op) {
  int num_spatial_dims = 2;
  int num_dims = 2 + num_spatial_dims;

  if (!IsOfRankOrUnranked(op.out_backprop(), num_dims) ||
      !IsOfRankOrUnranked(op.filter(), num_dims))
    return op.emitOpError()
           << "requires operands to be " << num_dims << "D tensor";

  LogicalResult verify_result = VerifyConvOpAttributes(op, num_dims);
  if (failed(verify_result)) {
    return verify_result;
  }

  return success();
}

LogicalResult Conv2DBackpropInputOp::UpdateDataFormat(StringRef data_format) {
  StringRef src_data_format = this->data_format();

  auto perm = GetDataFormatPermutation(src_data_format, data_format);
  if (perm.empty()) return failure();

  // Update data_format attribute and result types.
  if (failed(::mlir::TF::UpdateDataFormat(data_format, this))) return failure();

  // Update convolution attributes.
  setAttr("dilations", ShuffleArrayAttr(dilations(), perm));
  setAttr("strides", ShuffleArrayAttr(strides(), perm));
  setAttr("explicit_paddings", ShuffleArrayAttr(explicit_paddings(), perm, 2));

  // Permute input sizes operand.
  OpBuilder builder(getOperation());
  auto input_sizes_permuted = builder.create<TF::DataFormatVecPermuteOp>(
      getLoc(), input_sizes(), StringAttr::get(src_data_format, getContext()),
      StringAttr::get(data_format, getContext()));
  setOperand(0, input_sizes_permuted);

  return success();
}

StringRef Conv2DBackpropInputOp::GetOptimalLayout(
    const RuntimeDevices &devices) {
  // Keep current data format if no GPUs are available or if explicit placement
  // does not allow to use GPU for this operation.
  if (!CanUseGpuDevice(devices) || !CanUseGpuDevice(getOperation()))
    return data_format();

  // Filter must be a tensor.
  auto filter_ty = filter().getType().dyn_cast<TensorType>();
  if (!filter_ty) return data_format();

  // For f16 data type on devices with Tensor Cores support NHWC data format
  // is up to ~2x faster.
  const bool is_f16 = filter_ty.getElementType().isF16();
  if (is_f16 && CanUseTensorCores(devices)) return "NHWC";

  // Otherwise always use "NCHW".
  return "NCHW";
}

//===----------------------------------------------------------------------===//
// DataFormatVecPermuteOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(DataFormatVecPermuteOp op) {
  auto input_ty = op.x().getType().dyn_cast<RankedTensorType>();
  if (!input_ty) return success();

  int rank = input_ty.getRank();
  if (rank != 1 && rank != 2)
    return op.emitOpError("requires input of rank 1 or 2");

  if (rank == 1) {
    int64_t dim0 = input_ty.getDimSize(0);
    if (dim0 != ShapedType::kDynamicSize && dim0 != 4 && dim0 != 2)
      return op.emitOpError("requires 1D input of size 4 or size 2");
  }

  if (rank == 2) {
    int64_t dim0 = input_ty.getDimSize(0);
    if (dim0 != ShapedType::kDynamicSize && dim0 != 4)
      return op.emitOpError(
          "requires first dimensions of 2D input to be of size 4");

    int64_t dim1 = input_ty.getDimSize(1);
    if (dim1 != ShapedType::kDynamicSize && dim1 != 2)
      return op.emitOpError(
          "requires second dimensions of 2D input to be of size 2");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// DivOp
//===----------------------------------------------------------------------===//

void DivOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  results.insert<DivWithSqrtDivisor>(context);
}

OpFoldResult DivOp::fold(ArrayRef<Attribute> operands) {
  return IdentityArithmeticOpFolder<DivOp>(*this, operands);
}

//===----------------------------------------------------------------------===//
// DynamicStitchOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(DynamicStitchOp op) {
  if (op.N() < 1) return op.emitOpError("requires attribute N with value >= 1");

  if (RankedTensorType out_ty = op.getType().dyn_cast<RankedTensorType>()) {
    if (out_ty.getRank() == 0) {
      return op.emitOpError("requires non scalar output");
    }
  }

  llvm::SmallDenseSet<int64_t, 8> index_values;
  bool all_indices_const = true;
  int32_t max_index = -1;
  llvm::Optional<SmallVector<int64_t, 4>> inferred_item_shape;
  for (auto it : llvm::zip(op.indices(), op.data())) {
    Value index = std::get<0>(it);

    DenseIntElementsAttr index_attr;
    if (matchPattern(index, m_Constant(&index_attr))) {
      for (int32_t index : index_attr.getValues<int32_t>()) {
        if (index < 0)
          return op.emitOpError()
                 << "requires non-negative index values; found " << index;
        max_index = std::max(index, max_index);
        index_values.insert(index);
      }
    } else {
      all_indices_const = false;
    }

    Value data = std::get<1>(it);
    RankedTensorType index_ty = index.getType().dyn_cast<RankedTensorType>();
    RankedTensorType data_ty = data.getType().dyn_cast<RankedTensorType>();
    if (!index_ty || !data_ty) continue;

    int64_t index_rank = index_ty.getRank();
    ArrayRef<int64_t> data_shape = data_ty.getShape();
    ArrayRef<int64_t> index_shape = index_ty.getShape();
    if (failed(mlir::verifyCompatibleShape(index_shape,
                                           data_shape.take_front(index_rank))))
      return op.emitOpError() << "requires shape of data with type " << data_ty
                              << " to have prefix matching with shape of the "
                                 "corresponding index type "
                              << index_ty;

    ArrayRef<int64_t> item_shape = data_shape.drop_front(index_rank);
    if (!inferred_item_shape) {
      inferred_item_shape = llvm::to_vector<4>(item_shape);
      continue;
    }

    if (failed(mlir::verifyCompatibleShape(item_shape, *inferred_item_shape)))
      return op.emitOpError() << "has inconsistent shaped data and index "
                                 "pairs; inferred item shapes ["
                              << llvm::makeArrayRef(*inferred_item_shape)
                              << "] and [" << item_shape << "] don't match";
    for (int i = 0, e = item_shape.size(); i < e; ++i) {
      int64_t &inferred_dim = (*inferred_item_shape)[i];
      int64_t dim = item_shape[i];
      if (ShapedType::isDynamic(inferred_dim)) inferred_dim = dim;
    }
  }

  // If all indices are constants, then verify that they cover all indices in
  // the range [0, max_index] and the output type is legal.
  if (all_indices_const) {
    for (int32_t i = 0; i <= max_index; i++) {
      if (!index_values.count(i))
        return op.emitOpError() << "missing index " << i;
    }

    if (inferred_item_shape) {
      SmallVector<int64_t, 4> expected_shape;
      expected_shape.push_back(max_index + 1);
      expected_shape.append(inferred_item_shape->begin(),
                            inferred_item_shape->end());

      auto out_ty = op.getType().cast<TensorType>();
      auto expected_out_ty =
          RankedTensorType::get(expected_shape, out_ty.getElementType());

      if (!AreCastCompatible({out_ty, expected_out_ty})) {
        return op.emitOpError() << "has invalid output type; should be "
                                   "compatible with inferred type "
                                << expected_out_ty;
      }
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// EinsumOp
//===----------------------------------------------------------------------===//

// Verifies that,
// * Arity of the op is at most two.
//
// TODO(hinsu): Verify einsum equation attribute.
static LogicalResult Verify(EinsumOp op) {
  if (op.N() > 2) {
    return op.emitOpError("supports at most two operands");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// EmptyOp
//===----------------------------------------------------------------------===//

OpFoldResult EmptyOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.size() == 1 && "empty op has one operand");

  Attribute attr = operands.front();
  if (!attr) return {};

  auto int_attr = attr.cast<DenseIntElementsAttr>();
  SmallVector<int64_t, 6> out_shape;
  for (const auto val : int_attr.getValues<int32_t>()) {
    out_shape.push_back(val);
  }

  auto type = getResult().getType().cast<ShapedType>();
  auto etype = type.getElementType();

  // We can not fold if the result is not static.
  if (!type.hasStaticShape()) return {};

  if (auto float_type = etype.dyn_cast<FloatType>()) {
    auto out_type = RankedTensorType::get(out_shape, float_type);
    return DenseElementsAttr::get(out_type,
                                  {APFloat(float_type.getFloatSemantics())});
  }

  if (auto int_type = etype.dyn_cast<IntegerType>()) {
    auto out_type = RankedTensorType::get(out_shape, etype);
    APInt val(int_type.getWidth(), 0, int_type.getSignedness());
    return DenseElementsAttr::get(out_type, val);
  }

  return {};
}

//===----------------------------------------------------------------------===//
// EmptyTensorListOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(EmptyTensorListOp op) {
  if (!IsOfRankOrUnranked(op.element_shape(), 0) &&
      !IsOfRankOrUnranked(op.element_shape(), 1)) {
    return op.emitOpError("requires element_shape operand to be 0D/1D tensor");
  }

  if (!IsOfRankOrUnranked(op.max_num_elements(), 0)) {
    return op.emitOpError("requires max_num_elements operand to be 0D tensor");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// EqualOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(EqualOp op) {
  // If we allow inputs to have incompatible type, then nothing to do.
  if (!op.incompatible_shape_error()) return success();

  // Otherwise, check inputs are broadcastable.
  return mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(
      op.getOperation());
}

void EqualOp::build(OpBuilder &builder, OperationState &result, Value x,
                    Value y, BoolAttr incompatible_shape_error) {
  auto result_type = DeduceEqualCmpOpType(&builder, result.location, x, y,
                                          incompatible_shape_error);
  return build(builder, result, result_type, x, y, incompatible_shape_error);
}

//===----------------------------------------------------------------------===//
// ExpandDimsOp
//===----------------------------------------------------------------------===//

Type InferExpandDimsOpType(Value input, Value dim) {
  Type element_ty = input.getType().cast<TensorType>().getElementType();
  auto unranked_ty = UnrankedTensorType::get(element_ty);

  auto input_ty = input.getType().dyn_cast<RankedTensorType>();
  if (!input_ty) return unranked_ty;

  DenseIntElementsAttr dim_attr;
  if (!matchPattern(dim, m_Constant(&dim_attr)) ||
      dim_attr.getNumElements() != 1)
    return unranked_ty;
  int64_t dim_val = (*dim_attr.begin()).getSExtValue();
  int64_t input_rank = input_ty.getRank();

  if (dim_val < -input_rank - 1 || dim_val > input_rank + 1) return unranked_ty;
  if (dim_val < 0) dim_val += input_rank + 1;

  SmallVector<int64_t, 4> shape = llvm::to_vector<4>(input_ty.getShape());
  shape.insert(shape.begin() + dim_val, 1);
  return RankedTensorType::get(shape, element_ty);
}

void ExpandDimsOp::build(OpBuilder &builder, OperationState &result,
                         Value input, Value dim) {
  return build(builder, result, InferExpandDimsOpType(input, dim), input, dim);
}

//===----------------------------------------------------------------------===//
// FakeQuantWithMinMaxArgsOp
//===----------------------------------------------------------------------===//
static LogicalResult Verify(FakeQuantWithMinMaxArgsOp op) {
  // TODO(fengliuai): moving the following to an utility method.
  const llvm::fltSemantics &semantics = op.min().getSemantics();
  float rmin, rmax;
  if (&semantics == &APFloat::IEEEsingle()) {
    rmin = op.min().convertToFloat();
    rmax = op.max().convertToFloat();
  } else {
    rmin = op.min().convertToDouble();
    rmax = op.max().convertToDouble();
  }
  // Range boundaries must be valid.
  if (rmin >= rmax) {
    return op.emitOpError("range is invalid: [" + Twine(std::to_string(rmin)) +
                          "," + Twine(std::to_string(rmax)) + "]");
  }
  int64_t num_bits = op.num_bits().getSExtValue();
  if (num_bits < 2 || num_bits > 16) {
    return op.emitOpError(
        "requires num_bits to be between 2 and 16, inclusive");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// FakeQuantWithMinMaxVarsOp
//===----------------------------------------------------------------------===//
static LogicalResult Verify(FakeQuantWithMinMaxVarsOp op) {
  auto min = GetRankedTensorTypeForOperand(op.min());
  if (min && !IsOfRankedFloatTensorType(min, 0))
    return op.emitOpError("requires min to be a 0d float tensor");

  auto max = GetRankedTensorTypeForOperand(op.max());
  if (max && !IsOfRankedFloatTensorType(max, 0))
    return op.emitOpError("requires max to be a 0d float tensor");

  int64_t num_bits = op.num_bits().getSExtValue();
  if (num_bits < 2 || num_bits > 16) {
    return op.emitOpError(
        "requires num_bits to be between 2 and 16, inclusive");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// FakeQuantWithMinMaxVarsPerChannelOp
//===----------------------------------------------------------------------===//
static LogicalResult Verify(FakeQuantWithMinMaxVarsPerChannelOp op) {
  auto min = GetRankedTensorTypeForOperand(op.min());
  if (min && !IsOfRankedFloatTensorType(min, 1))
    return op.emitOpError("requires min to be a 1d float tensor");

  auto max = GetRankedTensorTypeForOperand(op.max());
  if (max && !IsOfRankedFloatTensorType(max, 1))
    return op.emitOpError("requires max to be a 1d float tensor");

  Value inputs = op.inputs();
  if (!HasRankAtLeast(inputs, 1))
    return op.emitError("requires inputs to be at least 1d float tensor");

  int64_t num_bits = op.num_bits().getSExtValue();
  if (num_bits < 2 || num_bits > 16) {
    return op.emitOpError(
        "requires num_bits to be between 2 and 16, inclusive");
  }

  auto inputs_type = inputs.getType().dyn_cast<RankedTensorType>();
  if (!inputs_type) return success();
  int depth = inputs_type.getDimSize(inputs_type.getRank() - 1);
  if ((min && min.getDimSize(0) != depth) ||
      (max && max.getDimSize(0) != depth)) {
    return op.emitOpError(
        "requires min and max to have same size as last dimension of inputs");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// FillOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(FillOp op) {
  if (!IsOfRankOrUnranked(op.dims(), 1))
    return op.emitOpError() << "requires dims to be a 1D tensor";
  if (!IsOfRankOrUnranked(op.value(), 0))
    return op.emitOpError() << "requires value to be a scalar";

  return success();
}

static ShapedType InferFillOpType(Value dims, Value value) {
  Type etype = value.getType().cast<ShapedType>().getElementType();

  DenseIntElementsAttr dims_attr;
  if (!matchPattern(dims, m_Constant(&dims_attr))) {
    return UnrankedTensorType::get(etype);
  }

  llvm::SmallVector<int64_t, 4> shape;
  shape.reserve(dims_attr.getNumElements());
  for (const APInt dim : dims_attr.getValues<APInt>()) {
    shape.push_back(dim.getSExtValue());
  }
  return RankedTensorType::get(shape, etype);
}

void FillOp::build(OpBuilder &builder, OperationState &result, Value dims,
                   Value value) {
  FillOp::build(builder, result, InferFillOpType(dims, value), dims, value);
}

OpFoldResult FillOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.size() == 2 && "fill op has two operand");

  auto type = getType().cast<ShapedType>();
  // DenseElementsAttr that is used in this folder only supports int and float
  // types.
  // TODO(hinsu): Handle complex types once there is a attribute kind for
  // complex.
  if (!type.getElementType().isIntOrFloat()) return {};

  auto value = operands[1].dyn_cast_or_null<ElementsAttr>();
  if (!value) return {};

  if (type.hasStaticShape())
    return DenseElementsAttr::get(type, value.getValue({}));

  auto dims = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!dims) return {};

  llvm::SmallVector<int64_t, 4> shape;
  shape.reserve(dims.getNumElements());
  for (const APInt dim : dims.getValues<APInt>()) {
    shape.push_back(dim.getSExtValue());
  }
  type = RankedTensorType::get(shape, type.getElementType());

  return DenseElementsAttr::get(type, value.getValue({}));
}

//===----------------------------------------------------------------------===//
// FusedBatchNormGradOp
//===----------------------------------------------------------------------===//

// TODO(b/150954845): Add benchmarks to verify that layout preference didn't
// change in the latest GPU generations.

LogicalResult FusedBatchNormGradV3Op::UpdateDataFormat(StringRef data_format) {
  return ::mlir::TF::UpdateDataFormat(data_format, this);
}

StringRef FusedBatchNormGradV3Op::GetOptimalLayout(
    const RuntimeDevices &devices) {
  // Keep current data format if no GPUs are available or if explicit placement
  // does not allow to use GPU for this operation.
  if (!CanUseGpuDevice(devices) || !CanUseGpuDevice(getOperation()))
    return data_format();

  // For f16 data type on devices with Tensor Cores support NHWC data format
  // is up to ~2x faster.
  auto x_ty = x().getType().cast<TensorType>();
  const bool is_f16 = x_ty.getElementType().isF16();
  if (is_f16 && CanUseTensorCores(devices)) return "NHWC";

  // For all other data types prefer NCHW.
  return "NCHW";
}

//===----------------------------------------------------------------------===//
// FusedBatchNormOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(FusedBatchNormOp op) {
  auto x = GetRankedTensorTypeForOperand(op.x());
  if (x && !IsOfRankedFloatTensorType(x, 4))
    return op.emitOpError("requires x to be a 4D float tensor");

  auto scale = GetRankedTensorTypeForOperand(op.scale());
  if (scale && !IsOfRankedFloatTensorType(scale, 1))
    return op.emitOpError("requires scale to be a 1D float tensor");

  auto offset = GetRankedTensorTypeForOperand(op.offset());
  if (offset && !IsOfRankedFloatTensorType(offset, 1))
    return op.emitOpError("requires offset to be a 1D float tensor");

  auto mean = GetRankedTensorTypeForOperand(op.mean());
  if (mean && !IsOfRankedFloatTensorType(mean, 1))
    return op.emitOpError("requires mean to be a 1D float tensor");

  auto variance = GetRankedTensorTypeForOperand(op.variance());
  if (variance && !IsOfRankedFloatTensorType(variance, 1))
    return op.emitOpError("requires variance to be a 1D float tensor");

  // TODO(antiagainst): check attributes

  return success();
}

//===----------------------------------------------------------------------===//
// FusedBatchNormV2Op / FusedBatchNormV3Op
//===----------------------------------------------------------------------===//

template <class Op>
static LogicalResult InferenceFoldOperandsPermutation(
    ArrayRef<int64_t> permutation, Op *op) {
  // FusedBatchNorm in training mode is a layout sentitive operation, and should
  // have already assigned an optimal data format.
  if (op->is_training()) return failure();
  return ::mlir::TF::FoldOperandsPermutation(permutation, op);
}

template <class Op>
static StringRef GetOptimalLayout(const RuntimeDevices &devices, Op *op) {
  // In inference mode FusedBatchNorm is not sensitive to data layout.
  if (!op->is_training()) return op->data_format();

  // Keep current data format if no GPUs are available or if explicit placement
  // does not allow to use GPU for this operation.
  if (!CanUseGpuDevice(devices) || !CanUseGpuDevice(op->getOperation()))
    return op->data_format();

  // For f16 data type on devices with Tensor Cores support NHWC data format
  // is up to ~2x faster.
  auto x_ty = op->x().getType().template cast<TensorType>();
  const bool is_f16 = x_ty.getElementType().isF16();
  if (is_f16 && CanUseTensorCores(devices)) return "NHWC";

  // For all other data types prefer NCHW.
  return "NCHW";
}

LogicalResult FusedBatchNormV2Op::FoldOperandsPermutation(
    ArrayRef<int64_t> permutation) {
  return ::mlir::TF::InferenceFoldOperandsPermutation(permutation, this);
}

LogicalResult FusedBatchNormV2Op::UpdateDataFormat(StringRef data_format) {
  return ::mlir::TF::UpdateDataFormat(data_format, this);
}

StringRef FusedBatchNormV2Op::GetOptimalLayout(const RuntimeDevices &devices) {
  return ::mlir::TF::GetOptimalLayout(devices, this);
}

LogicalResult FusedBatchNormV3Op::FoldOperandsPermutation(
    ArrayRef<int64_t> permutation) {
  return ::mlir::TF::InferenceFoldOperandsPermutation(permutation, this);
}

LogicalResult FusedBatchNormV3Op::UpdateDataFormat(StringRef data_format) {
  return ::mlir::TF::UpdateDataFormat(data_format, this);
}

StringRef FusedBatchNormV3Op::GetOptimalLayout(const RuntimeDevices &devices) {
  return ::mlir::TF::GetOptimalLayout(devices, this);
}

//===----------------------------------------------------------------------===//
// GatherV2Op
//===----------------------------------------------------------------------===//

static LogicalResult Verify(GatherV2Op op) {
  int64_t batch_dims = op.batch_dims().getSExtValue();
  if (auto ty = op.indices().getType().dyn_cast<RankedTensorType>()) {
    int64_t rank = ty.getRank();
    if (batch_dims > rank || batch_dims < -rank)
      return op.emitOpError()
             << "batch_dims (" << batch_dims << ") must be in range [" << -rank
             << ", " << rank + 1 << ")";
    if (batch_dims < 0) batch_dims += rank;
  }

  if (!HasRankAtMost(op.axis(), 1))
    return op.emitOpError("requires axis to have rank at most 1");

  DenseIntElementsAttr axis_attr;
  if (matchPattern(op.axis(), m_Constant(&axis_attr))) {
    int64_t axis = (*axis_attr.begin()).getSExtValue();
    if (auto ty = op.params().getType().dyn_cast<RankedTensorType>()) {
      int64_t rank = ty.getRank();
      if (axis >= rank || axis < -rank)
        return op.emitOpError() << "axis (" << axis << ") must be in range ["
                                << -rank << ", " << rank << ")";
      if (axis < 0) axis += rank;
    }

    if (batch_dims >= 0 && axis >= 0 && axis < batch_dims) {
      return op.emitOpError() << "requires axis (" << axis
                              << ") to be greater than or equal to batch_dims ("
                              << batch_dims << ")";
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// IfOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(IfOp op) {
  auto module = op.getParentOfType<ModuleOp>();
  auto then_fn = module.lookupSymbol<FuncOp>(op.then_branch());
  if (!then_fn)
    return op.emitOpError("then_branch refers to an undefined function : ")
           << op.then_branch();
  auto else_fn = module.lookupSymbol<FuncOp>(op.else_branch());
  if (!else_fn)
    return op.emitOpError("else_branch refers to an undefined function : ")
           << op.else_branch();
  auto then_fn_type = then_fn.getType();
  auto else_fn_type = else_fn.getType();

  // Non-conditional operands starting with the second operand are passed to
  // branches and should be pair-wise compatible with branches' inputs.
  unsigned expected_num_inputs = op.getNumOperands() - 1;
  if (then_fn_type.getNumInputs() != expected_num_inputs ||
      else_fn_type.getNumInputs() != expected_num_inputs)
    return op.emitError("branches should have " + Twine(expected_num_inputs) +
                        " inputs");

  for (unsigned i = 0; i < expected_num_inputs; ++i) {
    auto operand_type = op.getOperand(i + 1).getType().cast<TensorType>();
    auto then_input_type = then_fn_type.getInput(i).cast<TensorType>();
    if (!AreCastCompatible({operand_type, then_input_type}))
      return op.emitError(
          llvm::formatv("then branch input type {0} is incompatible with "
                        "operand type {1} at index {2}",
                        then_input_type, operand_type, i));

    auto else_input_type = else_fn_type.getInput(i).cast<TensorType>();
    if (!AreCastCompatible({operand_type, else_input_type}))
      return op.emitError(
          llvm::formatv("else branch input type {0} is incompatible with "
                        "operand type {1} at index {2}",
                        else_input_type, operand_type, i));

    // If branches have incompatible input types that means that no tensor can
    // serve as input to both the functions. Hence, the op is invalid.
    if (!AreCastCompatible({then_input_type, else_input_type}))
      return op.emitError(llvm::formatv(
          "branches inputs have incompatible types {0} and {1} at index {2}",
          then_input_type, else_input_type, i));
  }

  // Branches' results should be pair-wise compatible with the op results.
  unsigned expected_num_results = op.getNumResults();
  if (then_fn_type.getNumResults() != expected_num_results ||
      else_fn_type.getNumResults() != expected_num_results)
    return op.emitError("branches should have " + Twine(expected_num_results) +
                        " results");

  for (unsigned i = 0; i < expected_num_results; ++i) {
    auto result_type = op.getResult(i).getType().cast<TensorType>();
    auto then_result_type = then_fn_type.getResult(i).cast<TensorType>();
    if (!AreCastCompatible({then_result_type, result_type}))
      return op.emitError(
          llvm::formatv("then branch result type {0} is incompatible with op "
                        "result type {1} at index {2}",
                        then_result_type, result_type, i));

    auto else_result_type = else_fn_type.getResult(i).cast<TensorType>();
    if (!AreCastCompatible({else_result_type, result_type}))
      return op.emitError(
          llvm::formatv("else branch result type {0} is incompatible with op "
                        "result type {1} at index {2}",
                        else_result_type, result_type, i));
  }
  return success();
}

class FoldConstantIfOp : public OpRewritePattern<TF::IfOp> {
 public:
  explicit FoldConstantIfOp(MLIRContext *context)
      : OpRewritePattern<TF::IfOp>(context) {}
  LogicalResult matchAndRewrite(TF::IfOp op,
                                PatternRewriter &rewriter) const override;

 private:
  template <typename T>
  struct CallOpType {
    using CallOp = T;
  };
};

LogicalResult FoldConstantIfOp::matchAndRewrite(
    TF::IfOp op, PatternRewriter &rewriter) const {
  // Extract the constant cond value.
  DenseIntElementsAttr cond_attr;
  if (!matchPattern(op.cond(), m_Constant(&cond_attr))) return failure();

  // Cond value must be a scalar.
  if (cond_attr.getNumElements() != 1) return failure();

  // Select a branch function.
  bool cond = cond_attr.getSplatValue<BoolAttr>().getValue();
  FlatSymbolRefAttr func = cond ? op.then_branchAttr() : op.else_branchAttr();

  // Replace IfOp with PartitionedCallOp or StatefulPartitionedCallOp.
  auto rewrite = [&](auto op_type) {
    auto empty = rewriter.getStringAttr("");
    auto call_op = rewriter.create<typename decltype(op_type)::CallOp>(
        op.getLoc(), op.getResultTypes(), op.getOperands().drop_front(), func,
        /*config=*/empty, /*config_proto=*/empty, /*executor_type=*/empty);
    PropagateDeviceAndInternalAttrs(op.getOperation(), call_op);
    rewriter.replaceOp(op, call_op.getResults());
  };

  if (op.is_stateless())
    rewrite(CallOpType<PartitionedCallOp>{});
  else
    rewrite(CallOpType<StatefulPartitionedCallOp>{});

  return success();
}

void IfOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                       MLIRContext *context) {
  results.insert<FoldConstantIfOp>(context);
}

//===----------------------------------------------------------------------===//
// IfRegionOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(IfRegionOp op) {
  if (failed(VerifyRegionResults(op, op.then_branch(), "then")))
    return failure();
  if (failed(VerifyRegionResults(op, op.else_branch(), "else")))
    return failure();
  return success();
}

//===----------------------------------------------------------------------===//
// InvertOp
//===----------------------------------------------------------------------===//

void InvertOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                           MLIRContext *context) {
  results.insert<InvertNested>(context);
}

//===----------------------------------------------------------------------===//
// InvertPermutationOp
//===----------------------------------------------------------------------===//

// Verifies that the input is 1D.
static LogicalResult Verify(InvertPermutationOp op) {
  auto x_type = op.x().getType().cast<TensorType>();
  if (!x_type.hasRank()) return success();
  if (x_type.getShape().size() != 1)
    return op.emitOpError() << "requires input x to be 1-dimensional";

  return success();
}

//===----------------------------------------------------------------------===//
// LeakyReluOp
//===----------------------------------------------------------------------===//

OpFoldResult LeakyReluOp::fold(ArrayRef<Attribute> operands) {
  assert(operands.size() == 1 && "leaky relu has one operand");

  // leaky_relu(x, alpha: 1) -> x
  if (alpha().convertToFloat() == 1.0f) return getOperand();

  auto calculate = [&](FloatAttr arg) {
    APFloat val = arg.getValue();
    if (val.isNegative()) val = alpha() * val;
    return FloatAttr::get(arg.getType(), val);
  };

  if (auto arg = operands[0].dyn_cast_or_null<FloatAttr>()) {
    return calculate(arg);
  } else if (auto arg = operands[0].dyn_cast_or_null<SplatElementsAttr>()) {
    if (auto elementAttr = arg.getSplatValue().dyn_cast<FloatAttr>())
      return DenseElementsAttr::get(arg.getType(), calculate(elementAttr));
  }
  return {};
}

//===----------------------------------------------------------------------===//
// LogOp
//===----------------------------------------------------------------------===//

void LogOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  results.insert<LogOfSoftmax, LogToLog1p>(context);
}

//===----------------------------------------------------------------------===//
// LogicalNotOp
//===----------------------------------------------------------------------===//

void LogicalNotOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<LogicalNotNested, LogicalNotOfEqual, LogicalNotOfNotEqual,
                 LogicalNotOfGreater, LogicalNotOfGreaterEqual,
                 LogicalNotOfLess, LogicalNotOfLessEqual>(context);
}

//===----------------------------------------------------------------------===//
// MatrixBandPartOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(MatrixBandPartOp op) {
  if (!HasRankAtLeast(op.input(), 2)) {
    return op.emitOpError()
           << "requires `input` to have rank of at least 2, but found "
           << op.input().getType();
  }
  if (!IsOfRankOrUnranked(op.num_lower(), 0)) {
    return op.emitOpError()
           << "requires `num_lower` to have 0 dimensions, but found "
           << op.num_lower().getType();
  }
  if (!IsOfRankOrUnranked(op.num_upper(), 0)) {
    return op.emitOpError()
           << "requires `num_upper` to have 0 dimensions, but found "
           << op.num_upper().getType();
  }
  return success();
}

//===----------------------------------------------------------------------===//
// MaxOp
//===----------------------------------------------------------------------===//

void MaxOp::build(OpBuilder &builder, OperationState &result, Value input,
                  Value reduction_indices, BoolAttr keep_dims) {
  Type out_ty =
      InferReductionOpType(input, reduction_indices, keep_dims, &builder);
  build(builder, result, out_ty, input, reduction_indices, keep_dims);
}

//===----------------------------------------------------------------------===//
// MaxPoolOp
//===----------------------------------------------------------------------===//

LogicalResult MaxPoolOp::FoldOperandsPermutation(
    ArrayRef<int64_t> permutation) {
  return ::mlir::TF::FoldOperandsPermutation(
      permutation, this, {{"strides", strides()}, {"ksize", ksize()}});
}

//===----------------------------------------------------------------------===//
// MaxPoolGradOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(MaxPoolGradOp op) {
  if (!IsOfRankOrUnranked(op.orig_input(), 4)) {
    return op.emitOpError() << "requires orig_input to be rank 4";
  }
  if (!IsOfRankOrUnranked(op.orig_output(), 4)) {
    return op.emitOpError() << "requires orig_output to be rank 4";
  }
  if (!IsOfRankOrUnranked(op.grad(), 4)) {
    return op.emitOpError() << "requires grad to be rank 4";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// MeanOp
//===----------------------------------------------------------------------===//

LogicalResult MeanOp::FoldOperandsPermutation(ArrayRef<int64_t> permutation) {
  // Reduction indices must be defined by a constant operation.
  auto reduction_op =
      dyn_cast_or_null<TF::ConstOp>(reduction_indices().getDefiningOp());
  if (!reduction_op) return failure();

  auto reductions_value = reduction_op.value().dyn_cast<DenseElementsAttr>();
  if (!reductions_value) return failure();

  // Prepare new reduction indices according to operand permutation.
  SmallVector<int32_t, 4> shuffled_reduction;
  llvm::transform(reductions_value.getIntValues(),
                  std::back_inserter(shuffled_reduction),
                  [&](APInt idx) { return permutation[idx.getSExtValue()]; });

  // Add constant operation with a new reduction indices.
  OpBuilder builder(getOperation());
  auto type = mlir::RankedTensorType::get(shuffled_reduction.size(),
                                          builder.getIntegerType(32));
  auto values = mlir::DenseIntElementsAttr::get(type, shuffled_reduction);
  auto shuffled_reduction_op = builder.create<TF::ConstOp>(getLoc(), values);

  // Use new reduction indices.
  setOperand(1, shuffled_reduction_op);

  return success();
}

//===----------------------------------------------------------------------===//
// MulOp
//===----------------------------------------------------------------------===//

OpFoldResult MulOp::fold(ArrayRef<Attribute> operands) {
  return IdentityArithmeticOpFolder<MulOp>(*this, operands);
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_a_m.cc.inc"

}  // namespace TF
}  // namespace mlir
