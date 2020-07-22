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

#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_n_z.h"

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
// NegOp
//===----------------------------------------------------------------------===//

void NegOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  results.insert<NegNested>(context);
}

//===----------------------------------------------------------------------===//
// NotEqualOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(NotEqualOp op) {
  // If we allow inputs to have incompatible type, then nothing to do.
  if (!op.incompatible_shape_error()) return success();

  // Otherwise, check inputs are broadcastable.
  return mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(
      op.getOperation());
}

void NotEqualOp::build(OpBuilder &builder, OperationState &result, Value x,
                       Value y, BoolAttr incompatible_shape_error) {
  auto result_type = DeduceEqualCmpOpType(&builder, result.location, x, y,
                                          incompatible_shape_error);
  return build(builder, result, result_type, x, y, incompatible_shape_error);
}

//===----------------------------------------------------------------------===//
// OneHotOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(OneHotOp op) {
  int64_t axis = op.axis().getSExtValue();

  auto indices_ty = op.indices().getType().dyn_cast<RankedTensorType>();
  if (indices_ty &&
      !(axis == -1 || (axis >= 0 && axis <= indices_ty.getShape().size()))) {
    return op.emitOpError()
           << "expected axis (" << axis << ") to be -1 or between [0, "
           << indices_ty.getShape().size() << "]";
  }

  if (axis < -1) {
    return op.emitOpError() << "expected axis (" << axis
                            << ") to be -1 or between [0, rank(indices()))";
  }

  if (!IsOfRankOrUnranked(op.depth(), 0)) {
    return op.emitOpError() << "requires depth to be a scalar";
  }
  if (!IsOfRankOrUnranked(op.on_value(), 0)) {
    return op.emitOpError() << "requires on_value to be a scalar";
  }
  if (!IsOfRankOrUnranked(op.off_value(), 0)) {
    return op.emitOpError() << "requires off_value to be a scalar";
  }

  DenseIntElementsAttr depth_attr;
  if (matchPattern(op.depth(), m_Constant(&depth_attr))) {
    if (depth_attr.getType().getRank() != 0)
      return op.emitOpError() << "requires depth to be a scalar";
    int64_t depth = depth_attr.getValue<APInt>({}).getSExtValue();
    if (depth < 0) {
      return op.emitOpError() << "depth must be non-negative, got: " << depth;
    }
  }

  return success();
}

static TensorType InferOneHotOpType(Value indices, Value depth, Value on_value,
                                    Value off_value, IntegerAttr axis) {
  int64_t axis_val = axis.getInt();
  Type element_ty = on_value.getType().cast<TensorType>().getElementType();
  auto unranked_ty = UnrankedTensorType::get(element_ty);
  if (axis_val < -1) return unranked_ty;

  auto indices_ty = indices.getType().dyn_cast<RankedTensorType>();
  if (!indices_ty) return unranked_ty;

  auto shape = llvm::to_vector<2>(indices_ty.getShape());
  if (axis_val == -1) axis_val = shape.size();

  int64_t depth_val = ShapedType::kDynamicSize;
  DenseIntElementsAttr depth_attr;
  if (matchPattern(depth, m_Constant(&depth_attr)) &&
      depth_attr.getNumElements() == 1)
    depth_val = (*depth_attr.begin()).getSExtValue();
  shape.insert(shape.begin() + axis_val, depth_val);
  return RankedTensorType::get(shape, element_ty);
}

void OneHotOp::build(OpBuilder &builder, OperationState &result, Value indices,
                     Value depth, Value on_value, Value off_value,
                     IntegerAttr axis) {
  build(builder, result,
        InferOneHotOpType(indices, depth, on_value, off_value, axis), indices,
        depth, on_value, off_value, axis);
}

//===----------------------------------------------------------------------===//
// PackOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(PackOp op) {
  // TODO(hinsu): Convert variadic length attributes to derived attributes.
  Operation::operand_range values = op.values();

  if (failed(VerifyTypesCompatibility(values,
                                      /*mask_one_dim=*/false,
                                      op.getOperation()))) {
    return failure();
  }

  int64_t inputs_rank = -1;
  for (Value value : values) {
    if (auto ty = value.getType().dyn_cast<RankedTensorType>()) {
      // Exit early as input types are verified to be compatible so all ranked
      // tensors have the same rank.
      inputs_rank = ty.getRank();
      break;
    }
  }
  if (inputs_rank == -1) return success();

  // The values can be packed along any of the dimensions between 0 and
  // inputs rank, inclusive. Also, as the negative axis values wrap around so
  // the axis value range is [-(R+1), R+1).
  int64_t range_begin = -inputs_rank - 1;  // Inclusive
  int64_t range_end = inputs_rank + 1;     // Exclusive
  int64_t axis = op.axis().getSExtValue();
  if (axis < range_begin || axis >= range_end) {
    return op.emitError() << "attribute 'axis' should be within range ["
                          << range_begin << ", " << range_end
                          << "); actual value: " << axis;
  }

  return success();
}

//===----------------------------------------------------------------------===//
// PadOp
//===----------------------------------------------------------------------===//

LogicalResult PadOp::FoldOperandsPermutation(ArrayRef<int64_t> permutation) {
  // Paddings must be defined by a constant operation.
  auto paddings_op = dyn_cast_or_null<TF::ConstOp>(paddings().getDefiningOp());
  if (!paddings_op) return failure();

  auto paddings_value = paddings_op.value().dyn_cast<DenseElementsAttr>();
  if (!paddings_value ||
      paddings_value.getNumElements() != permutation.size() * 2)
    return failure();

  SmallVector<int32_t, 8> shuffled_paddings(paddings_value.getNumElements());
  for (auto index_pair : llvm::enumerate(paddings_value.getIntValues())) {
    size_t outer_idx = index_pair.index() / 2;
    size_t inner_idx = index_pair.index() % 2;

    shuffled_paddings[permutation[outer_idx] * 2 + inner_idx] =
        index_pair.value().getSExtValue();
  }

  // Add constant operation with a new paddings.
  OpBuilder builder(getOperation());
  auto type = mlir::RankedTensorType::get(paddings_value.getType().getShape(),
                                          builder.getIntegerType(32));
  auto values = mlir::DenseIntElementsAttr::get(type, shuffled_paddings);
  auto shuffled_paddings_op = builder.create<TF::ConstOp>(getLoc(), values);

  // Use new paddings.
  setOperand(1, shuffled_paddings_op);

  // Change the result type.
  getResult().setType(ShuffleRankedTensorType(getResult().getType(),
                                              ReversePermutation(permutation)));

  return success();
}

//===----------------------------------------------------------------------===//
// ParseExampleV2Op
//===----------------------------------------------------------------------===//

static LogicalResult Verify(ParseExampleV2Op op) {
  // NOTE(mrry): This validates properties of an op that would previously be
  // validated by the TensorFlow OpDef type checker. In addition to these
  // checks, the shape inference function for ParseExampleV2 validates the
  // consistency of the argument and result types.

  // Validate dense variadic input and output lengths.
  // NOTE(mrry): The Tdense attr is derived from dense_defaults, so we
  // do not need to validate dense_defaults.
  auto dense_types_count =
      std::distance(op.Tdense().begin(), op.Tdense().end());
  auto dense_values_count =
      std::distance(op.dense_values().begin(), op.dense_values().end());
  if (dense_values_count != dense_types_count) {
    return op.emitError() << "output 'dense_values' should have same length "
                          << "as attribute 'Tdense'";
  }

  // Validate sparse variadic output lengths.
  // NOTE(mrry): The sparse_types attr is derived from sparse_values, so we
  // do not need to validate sparse_values.
  auto sparse_types_count =
      std::distance(op.sparse_types().begin(), op.sparse_types().end());
  if (op.num_sparse() != sparse_types_count) {
    return op.emitError() << "attribute 'num_sparse' should be the same as "
                          << "the length of attribute 'sparse_types'";
  }
  if (op.sparse_indices().size() != sparse_types_count) {
    return op.emitError() << "output 'sparse_indices' should have same length "
                          << "as attribute 'sparse_types'";
  }
  if (op.sparse_shapes().size() != sparse_types_count) {
    return op.emitError() << "output 'sparse_shapes' should have same length "
                          << "as attribute 'sparse_types'";
  }

  // Validate ragged variadic output lengths.
  auto ragged_value_types_count = std::distance(op.ragged_value_types().begin(),
                                                op.ragged_value_types().end());
  auto ragged_split_types_count = std::distance(op.ragged_split_types().begin(),
                                                op.ragged_split_types().end());
  if (ragged_value_types_count != ragged_split_types_count) {
    return op.emitError() << "attribute 'ragged_value_types' should have same "
                          << "length as attribute 'ragged_split_types'";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// PartitionedCallOp
//===----------------------------------------------------------------------===//

template <class OpClass>
static LogicalResult VerifyPartitionedCall(OpClass op) {
  auto module = op.template getParentOfType<ModuleOp>();
  SymbolRefAttr func = op.getAttr("f").template cast<SymbolRefAttr>();

  auto function =
      dyn_cast_or_null<FuncOp>(SymbolTable::lookupSymbolIn(module, func));

  if (!function) {
    return op.emitError("'f' attribute refers to an undefined function: ")
           << func;
  }

  FunctionType function_ty = function.getType();
  int func_arg_count = function_ty.getNumInputs();
  int arg_count = op.args().size();

  if (arg_count != func_arg_count) {
    return op.emitError() << "argument count mismatch: 'args' has " << arg_count
                          << " arguments, but '" << func << "' expects "
                          << func_arg_count;
  }

  return success();
}

//===----------------------------------------------------------------------===//
// PowOp
//===----------------------------------------------------------------------===//

OpFoldResult PowOp::fold(ArrayRef<Attribute> operands) {
  auto constant_y = operands[1].dyn_cast_or_null<DenseFPElementsAttr>();
  if (constant_y && constant_y.isSplat()) {
    APFloat y_value = constant_y.getSplatValue<APFloat>();
    auto output_type = getType().cast<ShapedType>();
    if (y_value.isZero() && output_type.hasStaticShape()) {
      return DenseElementsAttr::get(
          output_type,
          FloatAttr::get(output_type.getElementType(), /*value=*/1.0));
    }
    if (y_value.isExactlyValue(1.0)) {
      return x();
    }
  }
  return {};
}

//===----------------------------------------------------------------------===//
// QrOp
//===----------------------------------------------------------------------===//

// Verifies that,
//
// * Input type, if ranked, must have at least 2 dimensions and at most
//   INT32_MAX dimensions.
//
static LogicalResult Verify(QrOp op) {
  auto ttype = op.input().getType().cast<TensorType>();
  if (!ttype.hasRank()) return success();
  if (!HasRankAtLeast(op.input(), 2))
    return op.emitOpError(
        "requires ranked input tensor to be of rank 2 or more");
  if (!HasRankAtMost(op.input(), std::numeric_limits<int32_t>::max()))
    return op.emitOpError(
        "requires ranked input tensor to be of rank INT32_MAX or less");

  return success();
}

//===----------------------------------------------------------------------===//
// ReadVariableOp
//===----------------------------------------------------------------------===//

void ReadVariableOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ReadVariableOfCast>(context);
}

//===----------------------------------------------------------------------===//
// ReciprocalOp
//===----------------------------------------------------------------------===//

void ReciprocalOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<ReciprocalNested>(context);
}

//===----------------------------------------------------------------------===//
// RandomUniformOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(RandomUniformOp op) {
  if (!IsOfRankOrUnranked(op.shape(), 1))
    return op.emitOpError("shape must be 1D tensor");
  return success();
}

//===----------------------------------------------------------------------===//
// RangeOp
//===----------------------------------------------------------------------===//

void RangeOp::build(OpBuilder &builder, OperationState &result, Value start,
                    Value limit, Value delta) {
  assert(start.getType() == limit.getType());
  assert(start.getType() == delta.getType());
  DenseIntElementsAttr start_val;
  DenseIntElementsAttr limit_val;
  DenseIntElementsAttr delta_val;
  if (matchPattern(start, m_Constant(&start_val)) &&
      matchPattern(limit, m_Constant(&limit_val)) &&
      matchPattern(delta, m_Constant(&delta_val))) {
    auto size = llvm::APIntOps::RoundingSDiv(
        *limit_val.begin() - *start_val.begin(), *delta_val.begin(),
        llvm::APInt::Rounding::DOWN);
    return RangeOp::build(
        builder, result,
        RankedTensorType::get(
            size.getSExtValue(),
            start.getType().cast<TensorType>().getElementType()),
        start, limit, delta);
  }
  return RangeOp::build(
      builder, result,
      RankedTensorType::get(
          {-1}, start.getType().cast<TensorType>().getElementType()),
      start, limit, delta);
}
//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

void RankOp::build(OpBuilder &builder, OperationState &result, Value input) {
  return RankOp::build(builder, result,
                       RankedTensorType::get({}, builder.getIntegerType(32)),
                       input);
}

// This will create a constant value for RankOp of a ranked tensor.
OpFoldResult RankOp::fold(ArrayRef<Attribute> operands) {
  auto type = input().getType();
  auto ranked_type = type.dyn_cast<RankedTensorType>();
  if (!ranked_type) return {};

  auto output_type = getType().cast<ShapedType>();
  int32_t rank = ranked_type.getRank();
  return DenseIntElementsAttr::get(output_type, rank);
}

//===----------------------------------------------------------------------===//
// RealDivOp
//===----------------------------------------------------------------------===//

void RealDivOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<RealDivWithSqrtDivisor, RealDivWithConstDivisor>(context);
}

OpFoldResult RealDivOp::fold(ArrayRef<Attribute> operands) {
  return IdentityArithmeticOpFolder<RealDivOp>(*this, operands);
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

// TODO(b/128020684): Verify the output type.
static LogicalResult Verify(ReshapeOp op) {
  auto shape_type = op.shape().getType().cast<TensorType>();
  if (!shape_type.hasRank()) return success();
  if (shape_type.getRank() != 1)
    return op.emitOpError("shape must be 1D tensor");
  auto rank_by_shape = shape_type.getShape()[0];
  auto type_of_tensor = op.tensor().getType().cast<TensorType>();
  // No compile time verification for unknown sized shape.
  if (rank_by_shape == -1 || !type_of_tensor.hasStaticShape()) return success();
  int64_t num_by_tensor = type_of_tensor.getNumElements();

  auto out_ty = op.getType().dyn_cast<RankedTensorType>();
  if (out_ty && out_ty.hasStaticShape()) {
    int64_t num_output_elements = out_ty.getNumElements();
    if (num_by_tensor != num_output_elements)
      return op.emitOpError()
             << "number of output elements (" << num_output_elements
             << ") does not match expected number of elements ("
             << num_by_tensor << ")";
  }

  // Check values if constant shape. No compiling time verification for
  // non-constant shape.
  auto *shape_op = op.shape().getDefiningOp();
  if (!shape_op) return success();
  Attribute shape_cst;
  if (!matchPattern(shape_op, m_Constant(&shape_cst))) return success();
  auto shape_cst_attr = shape_cst.dyn_cast<ElementsAttr>();
  if (!shape_cst_attr) return op.emitOpError("shape must be a valid tensor");

  if (auto opaque_attr = shape_cst_attr.dyn_cast<OpaqueElementsAttr>()) {
    opaque_attr.decode(shape_cst_attr);
  }

  // We know the shape is a 1-D Tensor, then let us get the number of
  // elements it implies.
  unsigned num_by_shape = 1;
  unsigned unknown_dim_count = 0;
  for (int i = 0, e = rank_by_shape; i != e; ++i) {
    auto num = shape_cst_attr.getValue<IntegerAttr>(i).getInt();
    // The dimension size value can be -1, and that the real size needs to
    // be computed so that the total size remains constant. At most one
    // component of shape can be -1.
    if (num == -1) {
      if (++unknown_dim_count > 1) {
        return op.emitOpError("more than one component of shape are -1");
      }
    } else {
      num_by_shape *= num;
    }
  }
  // If there is one component of shape is -1, the dimension should be
  // computed so that the total size remains constant.
  if (unknown_dim_count == 1) {
    if (num_by_tensor % num_by_shape != 0)
      return op.emitOpError(
          "one component of shape is -1 but couldn't infer the dimension");
    return success();
  }
  // If the elements by the tensor and implies by the shape don't match,
  // fail this static check.
  if (num_by_tensor != num_by_shape) {
    return op.emitOpError(
        "mismatch in tensor elements and shape implied elements");
  }
  return success();
}

void ReshapeOp::build(OpBuilder &builder, OperationState &result, Value tensor,
                      Value shape) {
  auto ttype = tensor.getType().cast<ShapedType>();
  auto etype = ttype.getElementType();

  auto unranked = [&builder, etype, &result, shape, tensor]() {
    return ReshapeOp::build(builder, result, UnrankedTensorType::get(etype),
                            tensor, shape);
  };

  // If tensor is unranked then we have no info about output of shape.
  if (!ttype.hasRank()) return unranked();

  DenseIntElementsAttr attr_shape;
  if (matchPattern(shape, m_Constant(&attr_shape))) {
    llvm::SmallVector<int64_t, 4> const_shape;
    const_shape.reserve(attr_shape.getNumElements());

    // Detect if reshape output shape is folded.
    bool flatten = false;
    int unknown_index = -1;
    // The product of constant shape argument excluding unknown dimension.
    int64_t product_cshape = 1;
    for (auto e : llvm::enumerate(attr_shape)) {
      int64_t val = e.value().getSExtValue();
      if (IsUnknownDimOrRank(val)) {
        if (flatten) {
          mlir::emitError(result.location)
              << "only one unknown dimension allowed";
          return;
        }
        flatten = true;
        unknown_index = e.index();
      } else {
        product_cshape *= val;
      }
      const_shape.push_back(val);
    }

    // Compute the value of the unknown dimension.
    if (flatten) {
      // Compute number of elements in tensor shape.
      auto tshape = ttype.getShape();
      int64_t product_tshape = std::accumulate(tshape.begin(), tshape.end(), 1,
                                               std::multiplies<int64_t>());
      // Set the unknown dimension such that total number of elements remain
      // constant.
      // Note: The case where the ratio is not integral, and so the total size
      // of reshape not constant, is checked in verify function.
      const_shape[unknown_index] = product_tshape / product_cshape;
    }
    return ReshapeOp::build(builder, result,
                            RankedTensorType::get(const_shape, etype), tensor,
                            shape);
  }
  return unranked();
}

void ReshapeOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                            MLIRContext *context) {
  results.insert<RedundantReshape>(context);
}

OpFoldResult ReshapeOp::fold(ArrayRef<Attribute> operands) {
  Value tensor = this->tensor();
  Value shape = this->shape();

  // Fold reshape if operand and result types are the same and all dimensions
  // are statically known (no-op reshape).
  // TODO(ezhulenev): Add the same folding for BroadcastToOp.
  auto result_ty = getType().dyn_cast<ShapedType>();
  if (result_ty && result_ty.hasStaticShape() &&
      result_ty == tensor.getType()) {
    return tensor;
  }

  // Fold reshape if the shape is computed from the input tensor:
  //
  //   %shape     = tf.Shape(%arg)                    // [? x ...]
  //   %dim0      = tf.StridedSlice(%shape, 0, 1, 1)  // get unknown dim value
  //   %new_shape = tf.Pack(dim0, ...) { axis = 0 }   // [? x ...]
  //   %reshape   = tf.Reshape(%arg, %new_shape)      // this is no-op
  //
  // Where `...` are some statically known dimensions. In this case reshape is
  // a no-op and can be replaced by %arg (assuming `...` are equal).
  auto pack_op = dyn_cast_or_null<PackOp>(shape.getDefiningOp());
  if (!pack_op || pack_op.values().size() < 2) return {};

  // Dimensions packed along axis = 0 (pack scalars into vector).
  if (pack_op.axis().getSExtValue() != 0) return {};

  // First packed value is defined by a strided slice operation.
  auto slice_op =
      dyn_cast_or_null<StridedSliceOp>(pack_op.values()[0].getDefiningOp());
  if (!slice_op) return {};

  // Input to the slice op is defined by shape operation.
  auto shape_op = dyn_cast_or_null<ShapeOp>(slice_op.input().getDefiningOp());
  if (!shape_op || shape_op.input() != tensor) return {};

  // All masks are `0` except `shrink_axis_mask` which is equal to `1` (slicing
  // scalar value from input vector).
  if (slice_op.begin_mask().getSExtValue() != 0 ||
      slice_op.ellipsis_mask().getSExtValue() != 0 ||
      slice_op.end_mask().getSExtValue() != 0 ||
      slice_op.new_axis_mask().getSExtValue() != 0 ||
      slice_op.shrink_axis_mask().getSExtValue() != 1)
    return {};

  // Returns a value if the `value` is defined by a ConstOp with a single
  // integer element in it and has an expected rank.
  auto get_value = [](Value value, int expected_rank) -> Optional<int64_t> {
    auto const_op = dyn_cast_or_null<ConstOp>(value.getDefiningOp());
    if (!const_op) return None;

    auto value_attr = const_op.value().dyn_cast<DenseIntElementsAttr>();
    if (!value_attr || value_attr.getNumElements() != 1) return None;

    auto value_ty = value_attr.getType();
    if (!value_ty.hasRank() || value_ty.getRank() != expected_rank) return None;

    auto splat = value_attr.getSplatValue<IntegerAttr>();
    return splat.getValue().getSExtValue();
  };

  // All other packed values are scalar constants.
  SmallVector<int64_t, 4> packed_dims;
  packed_dims.reserve(pack_op.values().size() - 1);
  for (Value operand : llvm::drop_begin(pack_op.values(), 1)) {
    if (auto dim = get_value(operand, /*expected_rank=*/0)) {
      packed_dims.push_back(*dim);
    } else {
      return {};
    }
  }

  // Slice exactly the first shape dimension:
  //   begin = [0] end = [1], strides = [1]
  auto begin = get_value(slice_op.begin(), /*expected_rank=*/1);
  auto end = get_value(slice_op.end(), /*expected_rank=*/1);
  auto strides = get_value(slice_op.strides(), /*expected_rank=*/1);
  if (!begin.hasValue() || !end.hasValue() || !strides.hasValue() ||
      *begin != 0 || *end != 1 || *strides != 1)
    return {};

  // First tensor dimension is dynamic.
  auto arg_ty = tensor.getType().dyn_cast<ShapedType>();
  if (!arg_ty || !arg_ty.hasRank() || arg_ty.getNumDynamicDims() != 1 ||
      !arg_ty.isDynamicDim(0))
    return {};

  // Argument tensor rank is equal to the number of packed dimensions.
  if (arg_ty.getRank() != pack_op.values().size()) return {};

  // All other dimensions are statically known and equal to packed dims.
  auto arg_dims = llvm::drop_begin(arg_ty.getShape(), 1);
  if (!std::equal(arg_dims.begin(), arg_dims.end(), packed_dims.begin()))
    return {};

  return tensor;
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

void SelectOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                           MLIRContext *context) {
  results.insert<SelectToSelectV2>(context);
}

// Verifies a few extra requirements on SelectOp:
// (1) `then` and `else` must have same shape
// (2) At least one of the following must be true:
//     (a) `cond` has the same rank as `then` and `else`
//     (b) `cond` is a scalar
//     (c) `cond` is a vector AND `then` and `else` are non-scalar with their
//         first dimension equal to `cond`.
static LogicalResult Verify(SelectOp op) {
  auto then_tensor = op.t().getType().cast<TensorType>();
  auto else_tensor = op.e().getType().cast<TensorType>();
  // Check (1).
  if (!AreCastCompatible({then_tensor, else_tensor}))
    return op.emitOpError() << "requires t and e have compatible shapes";

  // Get data rank (if exists).
  int data_rank;
  // If data is unranked or data_rank is 0, this will remain -2. Otherwise
  // refers to first dimension of then and/or else.
  int data_first_dim = -2;
  bool then_has_rank = then_tensor.hasRank();
  bool else_has_rank = else_tensor.hasRank();
  if (then_has_rank && else_has_rank) {
    data_rank = then_tensor.getRank();
    if (then_tensor.getRank() > 0)
      data_first_dim = then_tensor.getShape().front();
    if (else_tensor.getRank() > 0)
      data_first_dim = std::max(
          static_cast<int>(else_tensor.getShape().front()), data_first_dim);
  } else if (then_has_rank) {
    data_rank = then_tensor.getRank();
    if (then_tensor.getRank() > 0)
      data_first_dim = then_tensor.getShape().front();
  } else if (else_has_rank) {
    data_rank = else_tensor.getRank();
    if (else_tensor.getRank() > 0)
      data_first_dim = else_tensor.getShape().front();
  } else {
    // Neither has a rank.
    return success();
  }

  auto cond_tensor = op.condition().getType().dyn_cast<RankedTensorType>();
  if (!cond_tensor) return success();
  auto cond_rank = cond_tensor.getRank();
  // Check (2a) and (2b).
  if (cond_rank == 0 || cond_rank == data_rank) return success();
  // Check (2c).
  if (cond_rank == 1) {
    auto cond_shape = cond_tensor.getShape().front();
    if (data_rank == 0) {
      return op.emitOpError()
             << "requires that t and e are nonscalar when pred is a vector";
    }
    // We know `data` tensor has a rank of at least 1.
    if (data_first_dim != -1 && cond_shape != -1 &&
        data_first_dim != cond_shape) {
      return op.emitOpError() << "requires that, when pred is a vector, the "
                                 "shape matches the first dimension of t and e";
    }
    return success();
  }
  // None of (2a,b,c) were true; fail.
  return op.emitOpError() << "requires that pred is a scalar OR has the same "
                             "rank as t and e OR is a vector";
}

//===----------------------------------------------------------------------===//
// SelectV2Op
//===----------------------------------------------------------------------===//

static Type InferSelectV2OpType(Value condition, Value e, Value t) {
  Type element_ty = e.getType().cast<TensorType>().getElementType();
  auto unranked_ty = UnrankedTensorType::get(element_ty);

  Type broadcasted_ty =
      OpTrait::util::getBroadcastedType(e.getType(), t.getType());
  if (!broadcasted_ty) return unranked_ty;

  auto cond_ranked_ty = condition.getType().dyn_cast<RankedTensorType>();
  auto broadcasted_ranked_ty = broadcasted_ty.dyn_cast<RankedTensorType>();
  if (!cond_ranked_ty || !broadcasted_ranked_ty) return unranked_ty;

  // Explicitly get broadcasted output type as element types of condition may
  // not be same as the broadcated type's element type.
  SmallVector<int64_t, 4> result_shape;
  if (!OpTrait::util::getBroadcastedShape(cond_ranked_ty.getShape(),
                                          broadcasted_ranked_ty.getShape(),
                                          result_shape))
    return unranked_ty;
  return RankedTensorType::get(result_shape, element_ty);
}

void SelectV2Op::build(OpBuilder &builder, OperationState &result,
                       Value condition, Value e, Value t) {
  build(builder, result, InferSelectV2OpType(condition, e, t), condition, e, t);
}

//===----------------------------------------------------------------------===//
// ShapeOp
//===----------------------------------------------------------------------===//

namespace {
// Validates Shape/ShapeN/VariableShape operand and associated result types.
LogicalResult VerifyShapeOperandAndResult(Operation *op, Type operand_type,
                                          Type result_type,
                                          int variadic_idx = -1) {
  std::string variadic_idx_str =
      variadic_idx < 0 ? "" : llvm::formatv(" #{0}", variadic_idx).str();

  auto result_ranked_type = result_type.dyn_cast<RankedTensorType>();
  if (!result_ranked_type) return success();
  if (result_ranked_type.getShape().size() != 1)
    return op->emitOpError("requires 1D type for result") << variadic_idx_str;

  auto operand_ranked_type = operand_type.dyn_cast_or_null<RankedTensorType>();
  if (operand_ranked_type) {
    // The operand is a ranked tensor.
    if (result_ranked_type.hasStaticShape() &&
        !operand_ranked_type.getShape().empty() &&
        result_ranked_type.getDimSize(0) !=
            operand_ranked_type.getShape().size())
      return op->emitOpError("requires dimension size of result")
             << variadic_idx_str << " to match rank of operand"
             << variadic_idx_str;
  } else if (result_ranked_type.hasStaticShape()) {
    // The operand is an unranked tensor, print a warning if the result
    // is static.
    // Note: We do not handle this situation as an error, this would be too
    // restrictive due to incompleteness of shape inference at this point.
    op->emitWarning("has static shape result")
        << variadic_idx_str << " for unranked operand" << variadic_idx_str;
  }

  Type element_type = result_ranked_type.getElementType();
  if (!element_type.isSignlessInteger(32) &&
      !element_type.isSignlessInteger(64))
    return op->emitOpError("requires int32 or int64 return type for result")
           << variadic_idx_str;

  return success();
}
}  // anonymous namespace

static LogicalResult Verify(ShapeOp op) {
  return VerifyShapeOperandAndResult(op, op.input().getType(), op.getType());
}

// Converts shape of the given type to attribute if it is of ranked tensor type.
// Returned attribute has integer elements of the given width.
static Attribute ConvertShapeToAttr(Type input_ty, int out_width) {
  auto ranked_ty = input_ty.dyn_cast<RankedTensorType>();
  if (!ranked_ty || !ranked_ty.hasStaticShape()) return {};

  auto shape = ranked_ty.getShape();
  int rank = shape.size();

  SmallVector<APInt, 4> dimensions;
  dimensions.reserve(rank);
  for (int i = 0; i < rank; ++i)
    dimensions.push_back(APInt(out_width, shape[i]));

  auto result_type = RankedTensorType::get(
      {rank}, IntegerType::get(out_width, input_ty.getContext()));
  return DenseElementsAttr::get(result_type, dimensions);
}

OpFoldResult ShapeOp::fold(ArrayRef<Attribute> operands) {
  int width =
      getType().cast<ShapedType>().getElementType().getIntOrFloatBitWidth();
  return ConvertShapeToAttr(getOperand().getType(), width);
}

void ShapeOp::build(OpBuilder &builder, OperationState &result, Value input,
                    BoolAttr use32Bit) {
  auto rankedTensorType = input.getType().dyn_cast<RankedTensorType>();
  int64_t rank = rankedTensorType ? rankedTensorType.getRank() : -1;
  auto out_type = use32Bit.getValue() ? builder.getIntegerType(32)
                                      : builder.getIntegerType(64);
  return ShapeOp::build(builder, result,
                        RankedTensorType::get({rank}, out_type), input);
}

//===----------------------------------------------------------------------===//
// ShapeNOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(ShapeNOp op) {
  const size_t num_tensors = op.N();

  if (op.getNumOperands() != num_tensors)
    return op.emitOpError() << "requires " << num_tensors << " operand(s), got "
                            << op.getNumOperands() << " operand(s)";

  if (op.getNumResults() != num_tensors)
    return op.emitOpError() << "requires " << num_tensors << " result(s), got "
                            << op.getNumResults() << " result(s)";

  for (auto i : llvm::seq<uint64_t>(0, num_tensors)) {
    auto verification = VerifyShapeOperandAndResult(
        op, op.getOperand(i).getType(), op.getResult(i).getType(), i);
    if (failed(verification)) return verification;
  }

  return success();
}

LogicalResult ShapeNOp::fold(ArrayRef<Attribute> operands,
                             SmallVectorImpl<OpFoldResult> &results) {
  if (getNumOperands() == 0) return success();
  int width =
      getType(0).cast<ShapedType>().getElementType().getIntOrFloatBitWidth();

  for (Type input_ty : getOperandTypes()) {
    OpFoldResult result = ConvertShapeToAttr(input_ty, width);
    if (!result) return failure();

    results.push_back(result);
  }
  return success();
}

// TODO(hinsu): Add canonicalization pattern for ShapeN ops that don't have all
// static input shapes. Replacing output values corresponding to static input
// types may enable optimizations in users of the values.

//===----------------------------------------------------------------------===//
// SizeOp
//===----------------------------------------------------------------------===//

// Verifies that,
//
// * Input type, if is a ranked tensor, has at most INT32_MAX dimensions.
//
static LogicalResult Verify(SizeOp op) {
  if (!HasRankAtMost(op.input(), std::numeric_limits<int32_t>::max()))
    return op.emitOpError(
        "requires ranked input tensor to be of rank INT32_MAX or less");

  return success();
}

//===----------------------------------------------------------------------===//
// SliceOp
//===----------------------------------------------------------------------===//

// Verifies that:
//
// - operands begin and size are 1D with the same number of elements.
// - if the input is a ranked tensor, the rank of the input equals the number
//   of elements in operands begin and size.
// - if begin are constants, that
//   0 <= begin[i] <= begin[i] + size[i] <= input_ty.getShape()[i]
// - if begins aren't constant but the input is a ranked tensor, that
//   size[i] <= input_ty.getShape()[i]
//
static LogicalResult Verify(SliceOp op) {
  RankedTensorType begin_ty = GetRankedTensorTypeForOperand(op.begin());
  if (begin_ty && begin_ty.getRank() != 1) {
    return op.emitOpError() << "requires begin operand to be 1D tensor";
  }

  RankedTensorType size_ty = GetRankedTensorTypeForOperand(op.size());
  if (size_ty && size_ty.getRank() != 1) {
    return op.emitOpError() << "requires size operand to be 1D tensor";
  }

  if (!begin_ty || !size_ty || !begin_ty.hasStaticShape() ||
      !size_ty.hasStaticShape())
    return success();

  if (begin_ty.getNumElements() != size_ty.getNumElements()) {
    return op.emitOpError() << "requires begin and size operands to have the"
                               " same number of elements";
  }

  auto input_ty = op.input().getType().dyn_cast<RankedTensorType>();
  if (input_ty && begin_ty.getNumElements() != input_ty.getRank()) {
    return op.emitOpError() << "requires number of elements in begin and size"
                               "are equal to input rank";
  }

  DenseIntElementsAttr begin_indices;
  if (matchPattern(op.begin(), m_Constant(&begin_indices))) {
    DenseIntElementsAttr slice_sizes;
    bool constant_slice_sizes =
        matchPattern(op.size(), m_Constant(&slice_sizes));
    int dim = 0;
    for (const APInt &raw_begin_index : begin_indices.getValues<APInt>()) {
      int64_t begin_index = raw_begin_index.getSExtValue();
      int64_t input_size = input_ty ? input_ty.getShape()[dim] : -1;
      int64_t slice_size = constant_slice_sizes
                               ? slice_sizes.getValue<APInt>(dim).getSExtValue()
                               : 0;
      if (slice_size == -1 && input_size != -1) {
        slice_size = input_size - begin_index;
      }
      if (begin_index < 0 ||
          (input_size != -1 && begin_index + slice_size > input_size)) {
        return op.emitOpError()
               << "requires 0 <= begin[i] <= begin[i] + size[i] <= Di";
      }
      ++dim;
    }
  } else if (input_ty) {
    // If the inputs are ranked, we can do a few more sanity checks.
    DenseIntElementsAttr slice_sizes;
    if (matchPattern(op.size(), m_Constant(&slice_sizes))) {
      auto input_shape = input_ty.getShape();
      for (int64_t i = 0; i < input_ty.getRank(); ++i) {
        int64_t slice_size = slice_sizes.getValue<IntegerAttr>(i).getInt();
        int64_t input_size = input_shape[i];
        if (slice_size != -1 && input_size != -1 && slice_size > input_size) {
          return op.emitOpError() << "requires size[i] <= Di, even if begin[i] "
                                     "is unknown at compile time";
        }
      }
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// SoftmaxOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(SoftmaxOp op) {
  if (!HasRankAtLeast(op.logits(), 1)) {
    return op.emitOpError("requires operand to have rank at least 1");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SoftmaxCrossEntropyWithLogitsOp
//===----------------------------------------------------------------------===//

// Verifies that,
//
// * Input types are broadcast compatible and the broadcasted type has rank two.
//
static LogicalResult Verify(SoftmaxCrossEntropyWithLogitsOp op) {
  auto broadcasted_ty = OpTrait::util::getBroadcastedType(
                            op.features().getType(), op.labels().getType())
                            .dyn_cast_or_null<ShapedType>();
  if (!broadcasted_ty ||
      (broadcasted_ty.hasRank() && broadcasted_ty.getRank() != 2))
    return op.emitOpError(
        "requires features and labels to be broadcast compatible to rank two");

  return success();
}

//===----------------------------------------------------------------------===//
// SparseSoftmaxCrossEntropyWithLogitsOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(SparseSoftmaxCrossEntropyWithLogitsOp op) {
  if (!IsOfRankOrUnranked(op.features(), 2)) {
    return op.emitOpError("requires features operand of rank two");
  }
  if (!IsOfRankOrUnranked(op.labels(), 1)) {
    return op.emitOpError("requires labels operand of rank one");
  }
  auto features_ty = op.features().getType().dyn_cast<RankedTensorType>();
  auto labels_ty = op.labels().getType().dyn_cast<RankedTensorType>();
  if (features_ty && labels_ty) {
    int64_t features_batches = features_ty.getDimSize(0);
    int64_t labels_batches = labels_ty.getDimSize(0);
    if (!ShapedType::isDynamic(features_batches) &&
        !ShapedType::isDynamic(labels_batches) &&
        features_batches != labels_batches)
      return op.emitOpError(
          "requires features and labels with matching first dimension");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SplitOp
//===----------------------------------------------------------------------===//

// Verifies the input and split dimension operands for tf.Split/tf.SplitV.
// Writes the split dimension's index (adjusted with input rank) via `dim_index`
// if it's a constant.
template <class Op>
LogicalResult VerifySplitInputAndSplitDim(Op op, Optional<int64_t> *dim_index) {
  *dim_index = llvm::None;

  Value split_dim = op.split_dim();
  if (auto split_dim_type = split_dim.getType().dyn_cast<RankedTensorType>())
    if (split_dim_type.getRank() != 0)
      return op.emitOpError(
          "split dimension should be an integer scalar tensor");

  // We can perform further verification if the input tensor to be split has
  // known rank and the split dimension tensor is a constant.

  auto input_type = op.value().getType().template dyn_cast<RankedTensorType>();
  if (!input_type) return success();

  int64_t input_rank = input_type.getRank();
  if (input_rank == 0)
    return op.emitOpError("cannot split scalar input tensor");

  DenseIntElementsAttr split_dim_attr;
  if (!matchPattern(split_dim, m_Constant(&split_dim_attr))) return success();

  int64_t index = (*split_dim_attr.begin()).getSExtValue();

  if (index + input_rank < 0 || index >= input_rank) {
    return op.emitOpError("split dimension must be in range [-")
           << input_rank << ", " << input_rank << ")";
  }

  if (index < 0) index += input_rank;
  *dim_index = index;

  return success();
}

static LogicalResult Verify(SplitOp op) {
  Optional<int64_t> dim_index;
  if (failed(VerifySplitInputAndSplitDim(op, &dim_index))) return failure();
  if (!dim_index) return success();

  int64_t input_dim_size =
      op.value().getType().cast<RankedTensorType>().getDimSize(*dim_index);
  if (input_dim_size == ShapedType::kDynamicSize) return success();

  if (input_dim_size % op.getNumResults() != 0)
    return op.emitOpError("dimension #")
           << *dim_index << " not divisible by the number of result tensors";

  return success();
}

//===----------------------------------------------------------------------===//
// SplitVOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(SplitVOp op) {
  auto split_sizes_type =
      op.size_splits().getType().dyn_cast<RankedTensorType>();
  if (!split_sizes_type) return success();

  if (split_sizes_type.getRank() != 1 ||
      split_sizes_type.getDimSize(0) != op.getNumResults())
    return op.emitOpError("split sizes should be a 1D tensor of ")
           << op.getNumResults() << " elements";

  Optional<int64_t> dim_index = 0;
  if (failed(VerifySplitInputAndSplitDim(op, &dim_index))) return failure();
  if (!dim_index) return success();

  int64_t input_dim_size =
      op.value().getType().cast<RankedTensorType>().getDimSize(*dim_index);
  if (input_dim_size == ShapedType::kDynamicSize) return success();

  // If split sizes come from a constant, they must sum to the dimension size
  // along split_dim, and we can have no more than one dynamic dimension.
  DenseIntElementsAttr split_sizes_attr;
  if (!matchPattern(op.size_splits(), m_Constant(&split_sizes_attr)))
    return success();

  int64_t total_dim_size = 0;  // Total dimension size assigned to splits
  llvm::Optional<int> dynamic_dim_index;

  SmallVector<int64_t, 4> split_sizes;
  split_sizes.reserve(
      split_sizes_attr.getType().cast<ShapedType>().getNumElements());

  for (auto dim : llvm::enumerate(split_sizes_attr)) {
    int64_t dim_val = dim.value().getSExtValue();
    split_sizes.push_back(dim_val);
    if (dim_val == ShapedType::kDynamicSize) {
      // We cannot have more than one dynamic dimension.
      if (dynamic_dim_index)
        return op.emitOpError(
            "cannot have more than one dynamic dimension in split sizes");
      dynamic_dim_index = dim.index();
    } else {
      total_dim_size += dim_val;
    }
  }

  if (!dynamic_dim_index && total_dim_size != input_dim_size)
    return op.emitOpError(
               "split sizes must sum up to the dimension size along split "
               "dimension, found ")
           << total_dim_size << " vs " << input_dim_size;

  if (dynamic_dim_index && total_dim_size > input_dim_size)
    return op.emitOpError(
               "split sizes must sum up to be less than or equal to the "
               "dimension size along split dimension, found ")
           << total_dim_size << " vs " << input_dim_size;

  return success();
}

//===----------------------------------------------------------------------===//
// SquareOp
//===----------------------------------------------------------------------===//

void SquareOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                           MLIRContext *context) {
  results.insert<SquareOfSub>(context);
}

//===----------------------------------------------------------------------===//
// SubOp
//===----------------------------------------------------------------------===//

void SubOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                        MLIRContext *context) {
  results.insert<SubOfNeg>(context);
}

OpFoldResult SubOp::fold(ArrayRef<Attribute> operands) {
  return IdentityArithmeticOpFolder<SubOp>(*this, operands);
}

//===----------------------------------------------------------------------===//
// SumOp
//===----------------------------------------------------------------------===//

void SumOp::build(OpBuilder &builder, OperationState &result, Value input,
                  Value reduction_indices, BoolAttr keep_dims) {
  Type out_ty =
      InferReductionOpType(input, reduction_indices, keep_dims, &builder);
  build(builder, result, out_ty, input, reduction_indices, keep_dims);
}

//===----------------------------------------------------------------------===//
// StridedSliceOp
//===----------------------------------------------------------------------===//

// TODO(b/154160827): Add a canonicalization pattern from tf.StridedSliceOp to
// tf.SliceOp if both of the following are true:
// - All strides have a known value equal to 1
// - No masks are set (or masks can be applied by transforming the inputs to
//   Slice)

// Verifies that,
//
// - begin, end and strides operands are 1D and they have the same number of
//   elements. Here, the number of elements should be less than 32 to support
//   32-bit mask attributes.
// - None of the strides values are zero.
// - Ellipsis mask can have at most one bit set.

template <class OpTy>
static LogicalResult VerifyStridedSliceBase(OpTy op) {
  // Expected size for operands begin, end and strides vector operands.
  int64_t expected_size = -1;

  for (Value val : {op.begin(), op.end(), op.strides()}) {
    auto operand_ty = val.getType().dyn_cast<ShapedType>();
    if (!operand_ty || !operand_ty.hasStaticShape()) {
      // TensorFlow constant ops may have non-static shape because the shape is
      // not propagated during constant folding. If the defining op for this
      // operand is a constant op, use the constant op's attribute to get the
      // actual shape.
      DenseIntElementsAttr attr;
      if (!matchPattern(val, m_Constant(&attr))) continue;
      operand_ty = attr.getType();
    }

    if (operand_ty.getRank() != 1)
      return op.emitOpError()
             << "requires begin, end and strides to be 1D tensors";

    int64_t length = operand_ty.getDimSize(0);
    if (length == -1) continue;

    if (expected_size == -1) {
      // This op uses 32-bit masks.
      if (length >= 32)
        return op.emitOpError(
            "requires begin, end and strides operands with less than 32 "
            "elements");

      expected_size = length;
    } else if (length != expected_size) {
      return op.emitOpError() << "requires begin, end and strides to have the "
                                 "same number of elements";
    }
  }

  // If strides are constants, verify that none of the element is zero.
  DenseIntElementsAttr strides;
  if (matchPattern(op.strides(), m_Constant(&strides))) {
    if (llvm::is_contained(strides.getValues<APInt>(), 0))
      return op.emitOpError("requires non-zero strides");
  }

  // Use bit compares to ensure ellipsis_mask is 0 or a power of 2, i.e. there
  // exists only no more than one ellipsis.
  uint32_t ellipsis_mask = op.ellipsis_mask().getZExtValue();
  if (ellipsis_mask != 0 && !llvm::isPowerOf2_32(ellipsis_mask))
    return op.emitOpError("cannot have multiple ellipses");

  return success();
}

// Clamps the given `val`: returns `low` if `val` is less than `low`; returns
// `high` if `high` is less than `val`; otherwise returns `val`.
template <class T>
constexpr const T &Clamp(const T &val, const T &low, const T &high) {
  assert(!(high < low));
  return (val < low) ? low : (high < val) ? high : val;
}

// Checks if the `index` bit of `val` is set.
template <class T>
constexpr bool IsSet(const T &val, unsigned index) {
  return (val & (1 << index)) != 0;
}

// Sets the `index` bit of `val`.
template <class T>
constexpr void Set(T &val, unsigned index) {
  val |= (1 << index);
}

// Unset the `index` bit of `val`.
template <class T>
constexpr void Unset(T &val, unsigned index) {
  val &= ~(1 << index);
}

// Copy the `src_index` bit of `src` to `dst_index` bit of `dst`.
template <class T>
constexpr void CopyBit(const T &src, unsigned src_index, T &dst,
                       unsigned dst_index) {
  if (IsSet(src, src_index))
    Set(dst, dst_index);
  else
    Unset(dst, dst_index);
}

// The sparse spec of strided slice does not correspond to the number of
// dimensions. For example, sparse spec for foo[..., 3:10] for foo of shape (2,
// 4, 8) would have dims = 2.
struct SparseSliceSpec {
  int64_t dims;
  int32_t begin_mask, end_mask, ellipsis_mask, new_axis_mask, shrink_axis_mask;
  const ArrayRef<int64_t> &begin;
  const ArrayRef<int64_t> &end;
  const ArrayRef<int64_t> &strides;
};

// The dense spec of strided slice is the canonicalized version of sparse spec.
// The number of dimensions of dense spec correspond to the number of dimensions
// in operand tensor.
struct DenseSliceSpec {
  int64_t dims;
  int32_t begin_mask, end_mask, shrink_axis_mask;
  SmallVectorImpl<int64_t> &begin;
  SmallVectorImpl<int64_t> &end;
  SmallVectorImpl<int64_t> &strides;
};

// Make a sparse spec into a dense index spec.
// The sparse spec does not correspond to the number of dimensions
// Make a dense spec that corresponds to the number of dimensions
//
// For example suppose foo[...,3:, 2] on foo.shape=(2,2,3,4) then
// we need to produce the missing begin_mask, end_mask for the first two
// dimensions i.e. foo[:, :, 3:, 2].
static void BuildDenseSliceSpec(const SparseSliceSpec &sparse,
                                DenseSliceSpec *dense) {
  // Build expanded dense begin, end, strides, begin_mask, end_mask, and
  // shrink_axis_mask.
  dense->begin.resize(dense->dims);
  dense->end.resize(dense->dims);
  dense->strides.resize(dense->dims);
  dense->begin_mask = 0;
  dense->end_mask = 0;
  dense->shrink_axis_mask = 0;

  // Count number of new_axis after ellipsis. This helps in calculating the
  // number of dimensions ellipsis represents in the sparse spec.
  bool ellipsis_seen = false;
  int num_new_axis_after_ellipsis = 0;
  for (int sparse_index = 0; sparse_index < sparse.dims; ++sparse_index) {
    if (ellipsis_seen && IsSet(sparse.new_axis_mask, sparse_index))
      num_new_axis_after_ellipsis++;
    if (IsSet(sparse.ellipsis_mask, sparse_index)) ellipsis_seen = true;
  }

  int dense_index = 0;
  for (int sparse_index = 0; sparse_index < sparse.dims; ++sparse_index) {
    if (IsSet(sparse.new_axis_mask, sparse_index)) continue;
    if (IsSet(sparse.ellipsis_mask, sparse_index)) {
      auto next_index = std::min(dense->dims - (sparse.dims - sparse_index) +
                                     1 + num_new_axis_after_ellipsis,
                                 dense->dims);
      // Expand ellipsis into the appropriate dense indices. From current index
      // until next_index, all dimensions would have begin and end masks set and
      // stride 1, i.e., get all elements in those dimensions.
      for (; dense_index < next_index; ++dense_index) {
        dense->begin[dense_index] = dense->end[dense_index] = 0;
        dense->strides[dense_index] = 1;
        Set(dense->begin_mask, dense_index);
        Set(dense->end_mask, dense_index);
      }
      continue;
    }
    assert(dense_index < dense->dims);
    // Copy over the sparse indices to dense indices if ellipsis_mask and
    // new_axis_mask are not set.
    dense->begin[dense_index] = sparse.begin[sparse_index];
    dense->end[dense_index] = sparse.end[sparse_index];
    dense->strides[dense_index] = sparse.strides[sparse_index];
    CopyBit(sparse.begin_mask, sparse_index, dense->begin_mask, dense_index);
    CopyBit(sparse.end_mask, sparse_index, dense->end_mask, dense_index);
    CopyBit(sparse.shrink_axis_mask, sparse_index, dense->shrink_axis_mask,
            dense_index);
    dense_index++;
  }
}

// For the given `input_shape`, calculates the sliced shape using the given
// `begin`, `end`, and `stride` ranges and `begin_mask`, `end_mask`, and
// `shrink_axis_mask` masks. Updates the result back to `input_shape`. If
// `shrink_axis_mask` is not zero, this function will not drop the corresponding
// dimensions in `input_shape`; it will turn them into 1s. At the same time,
// canonicalizes `begin`, `end`, and `strides. The calculation follows
// tf.StridedSlice op semantics.
static void CalculateSlicedShapeFromDenseIndices(
    MutableArrayRef<int64_t> input_shape, int32_t begin_mask, int32_t end_mask,
    int32_t shrink_axis_mask, MutableArrayRef<int64_t> begin,
    MutableArrayRef<int64_t> end, MutableArrayRef<int64_t> stride) {
  assert(input_shape.size() <= 32);  // Only 32-bit masks are supported.

  // Make sure ranges' ranks are consistent with the input.
  assert(input_shape.size() == begin.size());
  assert(input_shape.size() == end.size());
  assert(input_shape.size() == stride.size());

  for (int i = 0, e = input_shape.size(); i < e; ++i) {
    if (ShapedType::isDynamic(input_shape[i])) continue;

    int64_t dim_i = input_shape[i];
    int64_t begin_i = begin[i];
    int64_t end_i = end[i];
    int64_t stride_i = stride[i];

    // [0]: mask for begin, [1]: mask for end
    int64_t masks[] = {begin_mask & (1 << i), end_mask & (1 << i)};
    // [0]: bound for begin, [1]: bound for end
    int64_t bounds[] = {stride_i > 0 ? 0 : -1,
                        stride_i > 0 ? dim_i : dim_i - 1};

    // Canonicalizes the given range `point` (begin/end) according to the
    // current dimension. `c` means case: 0 for begin, 1 for end.
    auto canonicalize = [&](int64_t point, int c) {
      if (masks[c]) return stride_i > 0 ? bounds[c] : bounds[(c + 1) & 1];

      // Add dim as offset to negative range point.
      point = point < 0 ? dim_i + point : point;
      return Clamp(point, bounds[0], bounds[1]);
    };

    begin_i = canonicalize(begin_i, 0);
    end_i = canonicalize(end_i, 1);

    int64_t interval_len = end_i - begin_i;
    int64_t size_i = 0;
    // If internal length is zero or has different sign from stride, it's a
    // degenerated case: we are slicing nothing. Otherwise, calculate the sliced
    // size.
    if (interval_len != 0 && (interval_len < 0) == (stride_i < 0))
      size_i = (interval_len / stride_i) + (interval_len % stride_i != 0);

    begin[i] = begin_i;
    if (IsSet(shrink_axis_mask, i)) {
      // Shrink this dimension. It means we only take the element at begin_i.
      input_shape[i] = 1;
      end[i] = begin_i + 1;
      stride[i] = 1;
    } else {
      input_shape[i] = size_i;
      end[i] = end_i;
      stride[i] = stride_i;
    }
  }
}

// For the given `input_shape`, calculates the sliced shape using the given
// `sparse_begin`, `sparse_end`, and `sparse_strides` ranges and `begin_mask`,
// `end_mask`, `ellipsis_mask` , `new_axis_mask` and `shrink_axis_mask` masks.
// Updates the result back to `input_shape`.
static void CalculateSlicedShapeFromSparseIndices(
    MutableArrayRef<int64_t> input_shape, ArrayRef<int64_t> sparse_begin,
    ArrayRef<int64_t> sparse_end, ArrayRef<int64_t> sparse_strides,
    int32_t begin_mask, int32_t end_mask, int32_t ellipsis_mask,
    int32_t new_axis_mask, int32_t shrink_axis_mask,
    SmallVectorImpl<int64_t> *begin, SmallVectorImpl<int64_t> *end,
    SmallVectorImpl<int64_t> *stride) {
  int64_t num_sparse_indices = sparse_begin.size();
  SparseSliceSpec sparse = {num_sparse_indices, begin_mask,    end_mask,
                            ellipsis_mask,      new_axis_mask, shrink_axis_mask,
                            sparse_begin,       sparse_end,    sparse_strides};

  // If no ellipsis_mask exists then an implicit ellipsis_mask at the end is
  // inserted. This handles cases where foo[2:4] (foo.shape() = [4, 8]) yields
  // a tensor of shape [2, 8], i.e., foo[2:4] is same as foo[2:4, ...].
  if (sparse.ellipsis_mask == 0) {
    Set(sparse.ellipsis_mask, sparse.dims);
    sparse.dims++;
  }

  int64_t dims = input_shape.size();
  DenseSliceSpec dense = {dims,
                          /*begin_mask = */ 0,
                          /*end_mask = */ 0,
                          /*shrink_axis_mask = */ 0,
                          *begin,
                          *end,
                          *stride};

  BuildDenseSliceSpec(sparse, &dense);
  CalculateSlicedShapeFromDenseIndices(input_shape, dense.begin_mask,
                                       dense.end_mask, dense.shrink_axis_mask,
                                       *begin, *end, *stride);
}

bool StridedSliceOp::GetSlicedBoundRanges(
    SmallVectorImpl<int64_t> *slice_begin, SmallVectorImpl<int64_t> *slice_end,
    SmallVectorImpl<int64_t> *slice_stride) {
  // TODO(hinsu): Support lowering for ops with dynamic begin and end values
  // when it is possible to derive indices based on mask attributes.
  DenseIntElementsAttr sparse_begin_attr, sparse_end_attr, sparse_strides_attr;
  if (!matchPattern(begin(), m_Constant(&sparse_begin_attr)) ||
      !matchPattern(end(), m_Constant(&sparse_end_attr)) ||
      !matchPattern(strides(), m_Constant(&sparse_strides_attr)))
    return false;

  auto input_ty = this->input().getType().dyn_cast<RankedTensorType>();
  if (!input_ty || !input_ty.hasStaticShape()) return false;
  auto input_shape = llvm::to_vector<4>(input_ty.getShape());

  SmallVector<int64_t, 4> sparse_begin, sparse_end, sparse_strides;

  for (const APInt &index : sparse_begin_attr)
    sparse_begin.push_back(index.getSExtValue());
  for (const APInt &index : sparse_end_attr)
    sparse_end.push_back(index.getSExtValue());
  for (const APInt &stride : sparse_strides_attr)
    sparse_strides.push_back(stride.getSExtValue());

  CalculateSlicedShapeFromSparseIndices(
      input_shape, sparse_begin, sparse_end, sparse_strides,
      begin_mask().getZExtValue(), end_mask().getZExtValue(),
      ellipsis_mask().getZExtValue(), new_axis_mask().getZExtValue(),
      shrink_axis_mask().getZExtValue(), slice_begin, slice_end, slice_stride);
  return true;
}

//===----------------------------------------------------------------------===//
// StridedSliceGradOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(StridedSliceGradOp op) {
  auto shape_type = op.shape().getType().dyn_cast<RankedTensorType>();
  if (shape_type && shape_type.getRank() != 1)
    return op.emitOpError("'shape' operand must be 1D tensor, but got ")
           << shape_type.getRank() << "D tensor";

  if (failed(VerifyStridedSliceBase(op))) return failure();

  // TODO(antiagainst): verify the gradient op.dy()'s shape is consistent with
  // the sliced type from StridedSlice.

  return success();
}

bool StridedSliceGradOp::GetSlicedShapeAndBoundRanges(
    SmallVectorImpl<int64_t> *input_shape,
    SmallVectorImpl<int64_t> *slice_begin, SmallVectorImpl<int64_t> *slice_end,
    SmallVectorImpl<int64_t> *slice_stride) {
  DenseIntElementsAttr shape_attr;
  DenseIntElementsAttr sparse_begin_attr, sparse_end_attr, sparse_strides_attr;
  if (!matchPattern(shape(), m_Constant(&shape_attr)) ||
      !matchPattern(begin(), m_Constant(&sparse_begin_attr)) ||
      !matchPattern(end(), m_Constant(&sparse_end_attr)) ||
      !matchPattern(strides(), m_Constant(&sparse_strides_attr)))
    return false;

  int rank = std::distance(shape_attr.begin(), shape_attr.end());

  input_shape->clear();
  input_shape->reserve(rank);
  for (const APInt &dim : shape_attr)
    input_shape->push_back(dim.getSExtValue());

  SmallVector<int64_t, 4> sparse_begin, sparse_end, sparse_strides;

  for (const APInt &index : sparse_begin_attr)
    sparse_begin.push_back(index.getSExtValue());
  for (const APInt &index : sparse_end_attr)
    sparse_end.push_back(index.getSExtValue());
  for (const APInt &stride : sparse_strides_attr)
    sparse_strides.push_back(stride.getSExtValue());

  CalculateSlicedShapeFromSparseIndices(
      *input_shape, sparse_begin, sparse_end, sparse_strides,
      begin_mask().getZExtValue(), end_mask().getZExtValue(),
      ellipsis_mask().getZExtValue(), new_axis_mask().getZExtValue(),
      shrink_axis_mask().getZExtValue(), slice_begin, slice_end, slice_stride);
  return true;
}

//===----------------------------------------------------------------------===//
// TensorListReserveOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TensorListReserveOp op) {
  if (!IsOfRankOrUnranked(op.element_shape(), 0) &&
      !IsOfRankOrUnranked(op.element_shape(), 1)) {
    return op.emitOpError("requires element_shape operand to be 0D/1D tensor");
  }

  if (!IsOfRankOrUnranked(op.num_elements(), 0)) {
    return op.emitOpError("requires num_elements operand to be 0D tensor");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// TensorListElementShapeOp
//===----------------------------------------------------------------------===//

OpFoldResult TensorListElementShapeOp::fold(ArrayRef<Attribute> operands) {
  int width =
      getType().cast<ShapedType>().getElementType().getIntOrFloatBitWidth();
  auto variant_type =
      getElementTypeOrSelf(getOperand().getType()).cast<TF::VariantType>();
  if (variant_type.getSubtypes().empty()) return {};
  return ConvertShapeToAttr(variant_type.getSubtypes()[0], width);
}

//===----------------------------------------------------------------------===//
// TensorListStackOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TensorListStackOp op) {
  if (!IsOfRankOrUnranked(op.element_shape(), 0) &&
      !IsOfRankOrUnranked(op.element_shape(), 1)) {
    return op.emitOpError("requires element_shape operand to be 0D/1D tensor");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// TensorScatterUpdateOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TensorScatterUpdateOp op) {
  if (!HasRankAtLeast(op.tensor(), 1))
    return op.emitOpError(
        "requires tensor operand to have at least 1 dimension");
  if (!HasRankAtLeast(op.indices(), 1))
    return op.emitOpError(
        "requires indices operand to have at least 1 dimension");
  if (!HasRankAtLeast(op.updates(), 1))
    return op.emitOpError(
        "requires updates operand to have at least 1 dimension");

  auto tensor_ty = op.tensor().getType().dyn_cast<RankedTensorType>();
  auto indices_ty = op.indices().getType().dyn_cast<RankedTensorType>();
  if (!tensor_ty || !indices_ty) return success();

  int64_t num_index_dims = indices_ty.getShape().back();
  if (ShapedType::isDynamic(num_index_dims)) return success();

  if (num_index_dims > tensor_ty.getRank())
    return op.emitOpError(
        "requires tensor operand with rank greater than or equal to the "
        "indices operand's last dimensions");
  return success();
}

//===----------------------------------------------------------------------===//
// TopKV2Op
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TopKV2Op op) {
  if (!HasRankAtLeast(op.input(), 1))
    return op.emitOpError(
        "requires input operand to have at least 1 dimension");

  if (!IsOfRankOrUnranked(op.k(), 0))
    return op.emitOpError("requires k operand to be 0D tensor");

  return success();
}

//===----------------------------------------------------------------------===//
// ToBoolOp
//===----------------------------------------------------------------------===//

namespace {
// If the input to ToBoolOp is a `tensor<i1>`, then the ToBoolOp is an identity
// function and can be removed.
class ToBoolOfZeroDBoolTensor : public OpRewritePattern<ToBoolOp> {
  using OpRewritePattern<ToBoolOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(ToBoolOp op,
                                PatternRewriter &rewriter) const override {
    if (auto type = op.getOperand().getType().dyn_cast<RankedTensorType>()) {
      if (type.getRank() == 0 && type.getElementType().isInteger(1)) {
        rewriter.replaceOp(op, op.getOperand());
        return success();
      }
    }
    return failure();
  }
};
}  // namespace

void ToBoolOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                           MLIRContext *context) {
  results.insert<ToBoolOfZeroDBoolTensor>(context);
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(TransposeOp op) {
  // TODO(hinsu): Verify using a custom verifier that,
  // * Transpose permutation is 1-D of size equal to the rank of the first
  //   input, if the shapes are partially known. Requires use of a more
  //   restrictive type than TF_Tensor.
  // * Result shape dimensions are possible based on the input shape.
  return success();
}

// TODO(jpienaar): perm could be optional too.
void TransposeOp::build(OpBuilder &builder, OperationState &result, Value x,
                        Value perm) {
  auto x_type = x.getType().cast<TensorType>();
  // If value is unranked, then so is results.
  if (!x_type.hasRank())
    return TransposeOp::build(builder, result,
                              UnrankedTensorType::get(x_type.getElementType()),
                              x, perm);

  // TODO(jpienaar): Handle unknown perm case.

  // TODO(jpienaar): Extract utility function.
  auto etype = x_type.cast<ShapedType>().getElementType();
  DenseIntElementsAttr attr_shape;
  if (matchPattern(perm, m_Constant(&attr_shape))) {
    llvm::SmallVector<int64_t, 4> const_shape;
    if (attr_shape.isSplat()) {
      const_shape.assign(
          attr_shape.getNumElements(),
          x_type.getDimSize((*attr_shape.begin()).getSExtValue()));
    } else {
      const_shape.reserve(attr_shape.getNumElements());
      for (const auto &dim : attr_shape)
        const_shape.push_back(x_type.getDimSize(dim.getSExtValue()));
    }
    return TransposeOp::build(
        builder, result, RankedTensorType::get(const_shape, etype), x, perm);
  }
  return TransposeOp::build(builder, result, UnrankedTensorType::get(etype), x,
                            perm);
}

namespace {

OpFoldResult FoldIdentityTranspose(TransposeOp op) {
  auto const_perm = dyn_cast_or_null<TF::ConstOp>(op.perm().getDefiningOp());
  if (!const_perm) return {};

  auto const_value = const_perm.value();
  const auto elements = const_value.getValues<APInt>();

  for (auto it : llvm::enumerate(elements)) {
    if (it.index() != it.value()) return {};
  }

  // TODO(jpienaar): Remove if/when we handle this more generally.
  if (op.getType() != op.x().getType()) {
    // If the types don't match then only fold if all the operands are in the TF
    // dialect.
    for (auto user : op.getOperation()->getUsers())
      if (user->getDialect() != op.getDialect()) return {};
  }

  return op.x();
}

OpFoldResult FoldCancellableTranspose(TransposeOp op) {
  // Operand is a TransposeOp.
  auto transpose = dyn_cast_or_null<TF::TransposeOp>(op.x().getDefiningOp());
  if (!transpose) return {};

  // Permutations defined by constant operations.
  auto perm0 = dyn_cast_or_null<TF::ConstOp>(op.perm().getDefiningOp());
  auto perm1 = dyn_cast_or_null<TF::ConstOp>(transpose.perm().getDefiningOp());
  if (!perm0 || !perm1) return {};

  // With permutation indices that cancel each other
  auto perm0_value = perm0.value().cast<DenseIntElementsAttr>();
  auto perm1_value = perm1.value().cast<DenseIntElementsAttr>();
  if (!AreCancellablePermutations(perm0_value, perm1_value)) return {};

  return transpose.x();
}

}  // namespace

OpFoldResult TransposeOp::fold(ArrayRef<Attribute> operands) {
  if (auto folded = FoldIdentityTranspose(*this)) return folded;
  if (auto folded = FoldCancellableTranspose(*this)) return folded;
  return {};
}

//===----------------------------------------------------------------------===//
// TruncateDivOp
//===----------------------------------------------------------------------===//

void TruncateDivOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<TruncateDivWithSqrtDivisor>(context);
}

//===----------------------------------------------------------------------===//
// UnpackOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(UnpackOp op) {
  auto value_type = op.value().getType().dyn_cast<RankedTensorType>();
  if (!value_type) return success();

  int64_t value_rank = value_type.getRank();
  int64_t axis = op.axis().getSExtValue();
  if (axis < -value_rank || axis >= value_rank)
    return op.emitOpError("axis attribute must be in the range of [-")
           << value_rank << ", " << value_rank << ')';

  axis = GetDimForAxis(axis, value_rank);
  int64_t dim_size = value_type.getDimSize(axis);
  if (ShapedType::isDynamic(dim_size)) return success();

  if (dim_size != op.getNumResults())
    return op.emitOpError("result count must be equal to ") << dim_size;

  return success();
}

//===----------------------------------------------------------------------===//
// Unsorted segment reduction ops
//===----------------------------------------------------------------------===//

template <class Op>
static LogicalResult VerifyUnsortedSegmentReduction(Op op) {
  if (!HasRankAtMost(op.num_segments(), 0))
    return op.emitOpError("number of segments should be a 0-D tensor");

  auto data_type = op.data().getType().template dyn_cast<RankedTensorType>();
  auto segment_ids_type =
      op.segment_ids().getType().template dyn_cast<RankedTensorType>();
  if (data_type && segment_ids_type) {
    if (data_type.getRank() < segment_ids_type.getRank())
      return op.emitOpError(
          "requires segment ids rank to be less than or equal to data's rank");

    int index = 0;
    for (auto shape_pair :
         llvm::zip_first(segment_ids_type.getShape(), data_type.getShape())) {
      int64_t segment_id_dim = std::get<0>(shape_pair);
      int64_t data_dim = std::get<1>(shape_pair);
      if (!ShapedType::isDynamic(segment_id_dim) &&
          !ShapedType::isDynamic(data_dim) && segment_id_dim != data_dim)
        return op.emitOpError(
                   "requires segment ids shape to be a prefix of data shape, "
                   "but dimension #")
               << index << " differs: " << segment_id_dim << " vs. "
               << data_dim;
      ++index;
    }
  }

  DenseIntElementsAttr num_segments_attr;
  if (matchPattern(op.num_segments(), m_Constant(&num_segments_attr))) {
    int64_t num_segments = (*num_segments_attr.begin()).getSExtValue();
    if (num_segments < 0)
      return op.emitOpError("num of segments cannot be negative");
  }

  return success();
}

//===----------------------------------------------------------------------===//
// VarIsInitializedOp
//===----------------------------------------------------------------------===//

namespace {

/// Erase VarIsInitializedOp operations with no uses. This op has side effect on
/// resources (read-only), but can still be deleted if it has zero uses.
struct EraseDeadVarIsInitializedOp
    : public OpRewritePattern<VarIsInitializedOp> {
  using OpRewritePattern<VarIsInitializedOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(VarIsInitializedOp op,
                                PatternRewriter &rewriter) const override {
    if (!op.use_empty()) return failure();
    rewriter.eraseOp(op);
    return success();
  }
};
}  // end anonymous namespace.

void VarIsInitializedOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<EraseDeadVarIsInitializedOp>(context);
}

//===----------------------------------------------------------------------===//
// VariableShapeOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(VariableShapeOp op) {
  auto input_type = op.input().getType().cast<TensorType>();
  if (input_type.hasStaticShape() && input_type.getNumElements() != 1)
    return op.emitOpError("requires input to have one resource");

  auto resource_type = input_type.getElementType().cast<TF::ResourceType>();
  auto subtypes = resource_type.getSubtypes();
  switch (subtypes.size()) {
    case 1:
      return VerifyShapeOperandAndResult(
          op, resource_type.getSubtypes().front(), op.getType());
    case 0:
      return VerifyShapeOperandAndResult(op, Type(), op.getType());
    default:
      return op.emitOpError(
          "requires resource input type to have at most 1 subtype");
  }
}

OpFoldResult VariableShapeOp::fold(ArrayRef<Attribute> operands) {
  int width =
      getType().cast<ShapedType>().getElementType().getIntOrFloatBitWidth();
  auto resource_type =
      getElementTypeOrSelf(getOperand().getType()).cast<TF::ResourceType>();
  if (resource_type.getSubtypes().empty()) return {};
  return ConvertShapeToAttr(resource_type.getSubtypes()[0], width);
}

//===----------------------------------------------------------------------===//
// WhileOp
//===----------------------------------------------------------------------===//

static LogicalResult Verify(WhileOp op) {
  auto module = op.getParentOfType<ModuleOp>();
  auto cond_fn = module.lookupSymbol<FuncOp>(op.cond());
  auto body_fn = module.lookupSymbol<FuncOp>(op.body());
  if (!cond_fn) {
    return op.emitOpError("cond refers to an undefined function : ")
           << op.cond();
  }
  if (!body_fn) {
    return op.emitOpError("body refers to an undefined function : ")
           << op.body();
  }

  auto cond_fn_type = cond_fn.getType();
  auto body_fn_type = body_fn.getType();

  // Verify that the cond function has exactly one result.
  if (cond_fn_type.getNumResults() != 1)
    return op.emitOpError("requires cond function to have exactly one result");

  SmallVector<Type, 4> operands(op.getOperandTypes());

  // Collect all the type lists for the op so that different pairs of type lists
  // can be compared for the compatibility.
  constexpr int kNumTypeLists = 5;
  const std::array<std::pair<std::string, ArrayRef<Type>>, kNumTypeLists>
      type_lists = {{
          {"operand", operands},
          {"body function result", body_fn_type.getResults()},
          {"result", op.getResultTypes()},
          {"cond function input", cond_fn_type.getInputs()},
          {"body function input", body_fn_type.getInputs()},
      }};

  // A pair of type lists should be cast compatible with each other if one is
  // converted to the another for a function call or assignment or there is a
  // common source of inputs for both.  Therefore, the While op requires the
  // following pairs of type lists to be cast compatible for the tensor_cast
  // operation:
  //
  // * Operands and cond inputs to call the cond function before the
  //   first iteration.
  // * Operands and body inputs to call the body function for the first
  //   iteration if the cond functions returns True or equivalent result.
  // * Operands and results to assign cond function arguments to op results if
  //   the cond function returns False or equivalent result.
  // * All three pairs using cond inputs, body inputs and results as operand is
  //   a common source for all three.
  // * Body result and cond inputs to call the cond function for the subsequent
  //   iterations. Similarly, Body result should be compatible with body inputs
  //   and op results.
  //
  // Note that the operands and body results need not be compatible as they are
  // never converted from one to the another nor there is a common source
  // tensors.  Compatibility requirement is not transitive.

  for (int i = 0; i < kNumTypeLists; ++i) {
    // Skip the first pair as the While op operands and body function results
    // does not need to be compatible with each other.
    for (int j = std::max(2, i + 1); j < kNumTypeLists; ++j) {
      auto &a = type_lists[i];
      auto &b = type_lists[j];

      int a_size = a.second.size();
      if (a_size != b.second.size())
        return op.emitOpError(
            llvm::formatv("requires the number of {0}s to be equal to the "
                          "number of {1}s. Found {2} and {3}, respectively",
                          a.first, b.first, a_size, b.second.size()));

      for (int idx = 0; idx < a_size; ++idx) {
        auto a_type = a.second[idx];
        auto b_type = b.second[idx];

        if (!AreCastCompatible({a_type, b_type}))
          return op.emitError(llvm::formatv(
              "{0} type {1} is incompatible with {2} type {3} at index {4}",
              a.first, a_type, b.first, b_type, idx));
      }
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// WhileRegionOp
//===----------------------------------------------------------------------===//
static LogicalResult Verify(WhileRegionOp op) {
  // Verify that the condition generates a single tensor<i1> result.
  YieldOp yield = cast<YieldOp>(op.cond().front().getTerminator());
  if (yield.getNumOperands() != 1)
    return op.emitOpError()
           << "condition should have a single tensor<i1> result";

  auto cond_type = yield.getOperand(0).getType().dyn_cast<RankedTensorType>();
  if (!cond_type || !cond_type.getShape().equals({}) ||
      !cond_type.getElementType().isInteger(/*width=*/1))
    return op.emitOpError()
           << "condition should have a single tensor<i1> result";

  // The body result types should match while op result types.
  if (failed(VerifyRegionResults(op, op.body(), "body"))) return failure();

  // Both condition and body should have same number and type of operands as
  // the WhileRegion inputs.
  const int num_inputs = op.getNumOperands();
  auto block_inputs_match_op_inputs = [&](Region &region,
                                          StringRef name) -> LogicalResult {
    Block &block = region.front();
    if (block.getNumArguments() != num_inputs)
      return op.emitOpError()
             << name << " should have same number of inputs (" << num_inputs
             << ") as " << WhileRegionOp::getOperationName() << " but has "
             << block.getNumArguments() << " inputs";

    for (auto types_idx : llvm::enumerate(
             llvm::zip(op.getOperandTypes(), block.getArgumentTypes()))) {
      auto op_input_type = std::get<0>(types_idx.value());
      auto block_input_type = std::get<1>(types_idx.value());
      if (!AreCastCompatible({block_input_type, op_input_type}))
        return op.emitOpError(llvm::formatv(
            "{0} input type {1} is incompatible with {2} "
            "input type {3} at index {4}",
            name, block_input_type, WhileRegionOp::getOperationName(),
            op_input_type, types_idx.index()));
    }
    return success();
  };

  if (failed(block_inputs_match_op_inputs(op.cond(), "condition")) ||
      failed(block_inputs_match_op_inputs(op.body(), "body")))
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// WhileRegionOp LoopLikeOpInterface
//===----------------------------------------------------------------------===//

Region &WhileRegionOp::getLoopBody() { return body(); }

bool WhileRegionOp::isDefinedOutsideOfLoop(Value value) {
  // If the Op defining the value exists and the defining op is outside the
  // scope of this WhileRegion, then we can infer that its defined outside.
  // The defining Op is outside the scope of this WhileRegion if this
  // WhileRegionOp is not an ancestor of the defining op in the parent chain.
  Operation *def_op = value.getDefiningOp();
  return def_op && !getOperation()->isAncestor(def_op);
}

LogicalResult WhileRegionOp::moveOutOfLoop(
    llvm::ArrayRef<mlir::Operation *> ops) {
  // Move the hoisted value to just before the while.
  Operation *while_op = this->getOperation();
  for (auto op : ops) op->moveBefore(while_op);
  return success();
}

//===----------------------------------------------------------------------===//
// WhileRegionOp canonicalization
//===----------------------------------------------------------------------===//
namespace {
// Eliminate values that pass through the WhileRegionOp body.
struct WhileRegionEliminatePassThrough
    : public OpRewritePattern<WhileRegionOp> {
  using OpRewritePattern<WhileRegionOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(WhileRegionOp while_op,
                                PatternRewriter &rewriter) const override {
    // Replace values that simply passthrough the body with extern values. The
    // block arguments of body and while match and so the corresponding cond
    // argument can be easily found.
    int old_num_operands = while_op.getNumOperands();
    int new_num_operands = old_num_operands;
    auto &body_block = while_op.body().front();
    auto &cond_block = while_op.cond().front();
    auto &yield = *body_block.getTerminator();

    // Bit mask indicating which operands will be removed.
    SmallVector<bool, 16> removed_operand(old_num_operands, false);

    for (int op_idx : llvm::seq<int>(0, old_num_operands)) {
      auto body_arg = body_block.getArgument(op_idx);
      if (body_arg == yield.getOperand(op_idx)) {
        // Replace the use of the passthrough value with the while operand
        // in the body and condition regions, as well as the while output (if
        // type match)
        // TODO(jurahul): Use PatternRewriter API for IR modification.
        auto value = while_op.getOperand(op_idx);
        if (body_arg.getType() == value.getType())
          body_arg.replaceAllUsesWith(value);

        auto cond_arg = cond_block.getArgument(op_idx);
        if (cond_arg.getType() == value.getType())
          cond_arg.replaceAllUsesWith(value);

        auto result = while_op.getResult(op_idx);
        if (result.getType() == value.getType())
          result.replaceAllUsesWith(value);
      }

      // Now check if the operand is unused in both regions as well as the
      // result. If so, mark it for removal.
      if (body_block.getArgument(op_idx).use_empty() &&
          cond_block.getArgument(op_idx).use_empty() &&
          while_op.getResult(op_idx).use_empty()) {
        removed_operand[op_idx] = true;
        new_num_operands--;
      }
    }

    if (new_num_operands == old_num_operands) return failure();

    // Compress the operands, region arguments, and outputs.
    SmallVector<Value, 4> new_while_operands;
    SmallVector<Type, 4> new_result_types;
    new_while_operands.reserve(new_num_operands);
    new_result_types.reserve(new_num_operands);

    // Build new operands and result type.
    int next_idx = 0;
    for (int op_idx : llvm::seq<int>(0, old_num_operands)) {
      if (removed_operand[op_idx]) continue;
      new_while_operands.push_back(while_op.getOperand(op_idx));
      new_result_types.push_back(while_op.getResult(op_idx).getType());
      next_idx++;
    }

    // Create the new while operation.
    auto new_while_op =
        rewriter.create<WhileRegionOp>(while_op.getLoc(), new_result_types,
                                       new_while_operands, while_op.getAttrs());

    // Move region bodies to the new while.
    rewriter.inlineRegionBefore(while_op.cond(), new_while_op.cond(),
                                new_while_op.cond().end());
    rewriter.inlineRegionBefore(while_op.body(), new_while_op.body(),
                                new_while_op.body().end());

    auto &new_cond_block = new_while_op.cond().front();
    auto &new_body_block = new_while_op.body().front();
    auto &new_yield = *new_body_block.getTerminator();

    // Build a vector of new results. Also patch up the region bodies and yield.
    SmallVector<Value, 4> new_results;
    next_idx = 0;
    for (int op_idx : llvm::seq<int>(0, old_num_operands)) {
      if (removed_operand[op_idx]) {
        new_cond_block.eraseArgument(next_idx);
        new_body_block.eraseArgument(next_idx);
        new_yield.eraseOperand(next_idx);
        new_results.push_back(nullptr);
      } else {
        new_results.push_back(new_while_op.getResult(next_idx++));
      }
    }

    rewriter.replaceOp(while_op, new_results);
    return success();
  }
};

}  // anonymous namespace

void WhileRegionOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<WhileRegionEliminatePassThrough>(context);
}

//===----------------------------------------------------------------------===//
// XdivyOp
//===----------------------------------------------------------------------===//

void XdivyOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                          MLIRContext *context) {
  results.insert<XdivyWithSqrtDivisor>(context);
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_n_z.cc.inc"

}  // namespace TF
}  // namespace mlir
