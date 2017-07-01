/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/compiler/xla/service/hlo_evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/index_util.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/map_util.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/ptr_util.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_query.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/window_util.h"
#include "tensorflow/core/lib/core/bitmap.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/types.h"

namespace xla {

namespace {

template <typename OperandT>
StatusOr<std::unique_ptr<Literal>> Compare(const Shape& shape, HloOpcode opcode,
                                           const Literal& lhs_literal,
                                           const Literal& rhs_literal) {
  std::function<bool(OperandT, OperandT)> compare_op;
  switch (opcode) {
    case HloOpcode::kEq:
      compare_op = [](OperandT lhs_el, OperandT rhs_el) {
        return lhs_el == rhs_el;
      };
      break;
    case HloOpcode::kNe:
      compare_op = [](OperandT lhs_el, OperandT rhs_el) {
        return lhs_el != rhs_el;
      };
      break;
    case HloOpcode::kGe:
      compare_op = [](OperandT lhs_el, OperandT rhs_el) {
        return lhs_el >= rhs_el;
      };
      break;
    case HloOpcode::kGt:
      compare_op = [](OperandT lhs_el, OperandT rhs_el) {
        return lhs_el > rhs_el;
      };
      break;
    case HloOpcode::kLe:
      compare_op = [](OperandT lhs_el, OperandT rhs_el) {
        return lhs_el <= rhs_el;
      };
      break;
    case HloOpcode::kLt:
      compare_op = [](OperandT lhs_el, OperandT rhs_el) {
        return lhs_el < rhs_el;
      };
      break;
    default:
      LOG(FATAL) << "unhandled HLO opcode for conversion to Comparison: "
                 << HloOpcodeString(opcode);
  }

  auto result = Literal::CreateFromShape(shape);
  TF_RETURN_IF_ERROR(result->Populate<bool>(
      [&](tensorflow::gtl::ArraySlice<int64> multi_index) {
        return compare_op(lhs_literal.Get<OperandT>(multi_index),
                          rhs_literal.Get<OperandT>(multi_index));
      }));

  return std::move(result);
}

template <typename ReturnT, typename NativeT>
StatusOr<std::unique_ptr<Literal>> ElementWiseUnaryOpImpl(
    HloInstruction* instruction,
    const std::function<ReturnT(NativeT)>& unary_op,
    const Literal& operand_literal) {
  const auto shape = instruction->shape();
  const auto* operand = instruction->operand(0);

  // TODO(b/35950897, b/27796129): add DCHECK back once implicit broadcast is
  // removed.
  if (!ShapeUtil::SameDimensions(shape, operand->shape())) {
    return Unimplemented(
        "Implicit broadcasting is currently unsupported in HLO evaluator "
        "Shape Mismatch: %s vs %s",
        ShapeUtil::HumanString(shape).c_str(),
        ShapeUtil::HumanString(operand->shape()).c_str());
  }

  auto result = Literal::CreateFromShape(shape);

  TF_RETURN_IF_ERROR(result->Populate<ReturnT>(
      [&](tensorflow::gtl::ArraySlice<int64> multi_index) {
        return unary_op(operand_literal.Get<NativeT>(multi_index));
      }));
  return std::move(result);
}

}  // namespace

template <typename ReturnT>
class HloEvaluator::TypedVisitor : public DfsHloVisitorWithDefault {
 public:
  explicit TypedVisitor(HloEvaluator* p) : parent_(p) {}

  Status DefaultAction(HloInstruction* hlo_instruction) override {
    return Unimplemented("unhandled HLO ops for HloEvaluator: %s.",
                         HloOpcodeString(hlo_instruction->opcode()).c_str());
  };

  // TODO(b/35950897): many of the stl functions used in the handlers are not
  // overloaded for every XLA primitive types.

  template <typename NativeT,
            typename std::enable_if<std::is_unsigned<NativeT>::value>::type* =
                nullptr>
  Status HandleAbs(HloInstruction* abs, HloInstruction* operand) {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[abs],
                        ElementWiseUnaryOp(abs, [](NativeT elem_operand) {
                          return elem_operand;
                        }));
    return Status::OK();
  }

  template <
      typename NativeT,
      typename std::enable_if<std::is_signed<NativeT>::value>::type* = nullptr>
  Status HandleAbs(HloInstruction* abs, HloInstruction* operand) {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[abs],
                        ElementWiseUnaryOp(abs, [](NativeT elem_operand) {
                          return std::abs(elem_operand);
                        }));
    return Status::OK();
  }

  Status HandleAbs(HloInstruction* abs, HloInstruction* operand) override {
    return HandleAbs<ReturnT>(abs, operand);
  };

  Status HandleBroadcast(HloInstruction* broadcast) override {
    parent_->evaluated_[broadcast] =
        Literal::CreateFromShape(broadcast->shape());
    auto output = parent_->evaluated_[broadcast].get();
    auto operand_to_broadcast =
        parent_->GetEvaluatedLiteralFor(broadcast->operand(0));
    std::vector<int64> broadcast_indices(
        ShapeUtil::Rank(broadcast->operand(0)->shape()), 0);
    return output->Populate<ReturnT>(
        [&](tensorflow::gtl::ArraySlice<int64> multi_index) {
          for (int64 i = 0; i < broadcast->dimensions().size(); ++i) {
            broadcast_indices[i] = multi_index[broadcast->dimensions(i)];
          }
          return operand_to_broadcast.Get<ReturnT>(broadcast_indices);
        });
  }

  Status HandleCeil(HloInstruction* ceil, HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[ceil],
                        ElementWiseUnaryOp(ceil, [](ReturnT elem_operand) {
                          return std::ceil(elem_operand);
                        }));
    return Status::OK();
  };

  Status HandleCopy(HloInstruction* copy) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[copy],
                        ElementWiseUnaryOp(copy, [](ReturnT elem_operand) {
                          return elem_operand;
                        }));
    return Status::OK();
  };

  Status HandleConvert(HloInstruction* convert) override {
    const HloInstruction* operand = convert->operand(0);
    TF_RET_CHECK(ShapeUtil::SameDimensions(operand->shape(), convert->shape()));
    TF_ASSIGN_OR_RETURN(std::unique_ptr<Literal> result,
                        parent_->GetEvaluatedLiteralFor(operand).Convert(
                            convert->shape().element_type()));

    if (LayoutUtil::LayoutsInShapesEqual(result->shape(), convert->shape())) {
      parent_->evaluated_[convert] = std::move(result);
    } else {
      parent_->evaluated_[convert] =
          result->Relayout(convert->shape().layout());
    }
    return Status::OK();
  }

  Status HandleExp(HloInstruction* exp, HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[exp],
                        ElementWiseUnaryOp(exp, [](ReturnT elem_operand) {
                          return std::exp(elem_operand);
                        }));
    return Status::OK();
  };

  Status HandleFloor(HloInstruction* floor, HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[floor],
                        ElementWiseUnaryOp(floor, [](ReturnT elem_operand) {
                          return std::floor(elem_operand);
                        }));
    return Status::OK();
  };

  Status HandleLog(HloInstruction* log, HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[log],
                        ElementWiseUnaryOp(log, [](ReturnT elem_operand) {
                          return std::log(elem_operand);
                        }));
    return Status::OK();
  };

  Status HandleLogicalNot(HloInstruction* logical_not,
                          HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[logical_not],
        ElementWiseUnaryOp(logical_not,
                           [](ReturnT elem_operand) { return !elem_operand; }));
    return Status::OK();
  };

  Status HandleNegate(HloInstruction* negate,
                      HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[negate],
                        ElementWiseUnaryOp(negate, [](ReturnT elem_operand) {
                          return -elem_operand;
                        }));
    return Status::OK();
  };

  Status HandleSign(HloInstruction* sign, HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[sign],
                        ElementWiseUnaryOp(sign, [](ReturnT elem_operand) {
                          return (ReturnT(0) < elem_operand) -
                                 (elem_operand < ReturnT(0));
                        }));
    return Status::OK();
  };

  Status HandleTanh(HloInstruction* tanh, HloInstruction* operand) override {
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[tanh],
                        ElementWiseUnaryOp(tanh, [](ReturnT elem_operand) {
                          return std::tanh(elem_operand);
                        }));
    return Status::OK();
  };

  Status HandleMultiply(HloInstruction* multiply, HloInstruction* lhs,
                        HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[multiply],
        ElementWiseBinaryOp(multiply, [](ReturnT lhs_elem, ReturnT rhs_elem) {
          return lhs_elem * rhs_elem;
        }));
    return Status::OK();
  };

  Status HandleSubtract(HloInstruction* subtract, HloInstruction* lhs,
                        HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[subtract],
        ElementWiseBinaryOp(subtract, [](ReturnT lhs_elem, ReturnT rhs_elem) {
          return lhs_elem - rhs_elem;
        }));
    return Status::OK();
  };

  Status HandleAdd(HloInstruction* add, HloInstruction* lhs,
                   HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[add],
        ElementWiseBinaryOp(add, [](ReturnT lhs_elem, ReturnT rhs_elem) {
          return lhs_elem + rhs_elem;
        }));
    return Status::OK();
  };

  Status HandleDivide(HloInstruction* divide, HloInstruction* lhs,
                      HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[divide],
        ElementWiseBinaryOp(divide, [](ReturnT lhs_elem, ReturnT rhs_elem) {
          return lhs_elem / rhs_elem;
        }));
    return Status::OK();
  };

  Status HandleMaximum(HloInstruction* maximum) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[maximum],
        ElementWiseBinaryOp(maximum, [](ReturnT lhs, ReturnT rhs) {
          return std::fmax(lhs, rhs);
        }));
    return Status::OK();
  };

  Status HandleMinimum(HloInstruction* minimum) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[minimum],
        ElementWiseBinaryOp(minimum, [](ReturnT lhs_el, ReturnT rhs_el) {
          return std::fmin(lhs_el, rhs_el);
        }));
    return Status::OK();
  };

  Status HandlePower(HloInstruction* power, HloInstruction* lhs,
                     HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[power],
        ElementWiseBinaryOp(power, [](ReturnT lhs_el, ReturnT rhs_el) {
          return std::pow(lhs_el, rhs_el);
        }));
    return Status::OK();
  };

  Status HandleRemainder(HloInstruction* remainder, HloInstruction* lhs,
                         HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[remainder],
        ElementWiseBinaryOp(remainder, [](ReturnT lhs_el, ReturnT rhs_el) {
          return std::fmod(lhs_el, rhs_el);
        }));
    return Status::OK();
  };

  Status HandleLogicalAnd(HloInstruction* logical_and, HloInstruction* lhs,
                          HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[logical_and],
        ElementWiseBinaryOp(logical_and, [](ReturnT lhs_el, ReturnT rhs_el) {
          return lhs_el && rhs_el;
        }));
    return Status::OK();
  };

  Status HandleLogicalOr(HloInstruction* logical_or, HloInstruction* lhs,
                         HloInstruction* rhs) override {
    TF_ASSIGN_OR_RETURN(
        parent_->evaluated_[logical_or],
        ElementWiseBinaryOp(logical_or, [](ReturnT lhs_el, ReturnT rhs_el) {
          return lhs_el || rhs_el;
        }));
    return Status::OK();
  };

  Status HandleClamp(HloInstruction* clamp, HloInstruction* min,
                     HloInstruction* arg, HloInstruction* max) override {
    std::function<ReturnT(ReturnT, ReturnT, ReturnT)> clamp_op =
        [](ReturnT low, ReturnT high, ReturnT value) {
          return std::fmax(low, std::fmin(value, high));
        };
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[clamp],
                        ElementWiseTernaryOp(clamp, std::move(clamp_op)));
    return Status::OK();
  };

  Status HandleSelect(HloInstruction* select, HloInstruction* pred,
                      HloInstruction* on_true,
                      HloInstruction* on_false) override {
    CHECK(!ShapeUtil::IsTuple(select->shape()));
    std::function<ReturnT(bool, ReturnT, ReturnT)> select_op =
        [](bool pred, ReturnT on_true, ReturnT on_false) {
          if (pred) {
            return on_true;
          }
          return on_false;
        };
    TF_ASSIGN_OR_RETURN(parent_->evaluated_[select],
                        ElementWiseTernaryOp(select, std::move(select_op)));
    return Status::OK();
  };

  Status HandleConvolution(HloInstruction* conv, HloInstruction* lhs,
                           HloInstruction* rhs, const Window& window) override {
    CHECK(ShapeUtil::IsArray(lhs->shape()));
    CHECK(ShapeUtil::IsArray(rhs->shape()));
    CHECK(ShapeUtil::SameElementType(lhs->shape(), rhs->shape()));
    CHECK(ShapeUtil::SameElementType(lhs->shape(), conv->shape()));
    TF_CHECK_OK(ShapeUtil::ValidateShape(lhs->shape()));
    TF_CHECK_OK(ShapeUtil::ValidateShape(rhs->shape()));

    const auto& dnums = conv->convolution_dimension_numbers();
    const int64 num_spatial_dims = dnums.spatial_dimensions_size();
    CHECK_EQ(num_spatial_dims, dnums.kernel_spatial_dimensions_size());
    CHECK_GE(num_spatial_dims, 1);
    CHECK_EQ(window.dimensions_size(), num_spatial_dims);

    CHECK_EQ(num_spatial_dims + 2, ShapeUtil::Rank(lhs->shape()));
    CHECK_EQ(num_spatial_dims + 2, ShapeUtil::Rank(rhs->shape()));

    TF_ASSIGN_OR_RETURN(auto inferred_return_shape,
                        ShapeInference::InferConvolveShape(
                            lhs->shape(), rhs->shape(), window, dnums));
    CHECK(ShapeUtil::Compatible(conv->shape(), inferred_return_shape))
        << "return shape set to: " << ShapeUtil::HumanString(conv->shape())
        << " but is inferred to be: "
        << ShapeUtil::HumanString(inferred_return_shape);

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);

    const auto lhs_rank = ShapeUtil::Rank(lhs->shape());
    const auto rhs_rank = ShapeUtil::Rank(rhs->shape());

    // Dimension number applicable for both input (lhs), and output.
    const int64 batch_dim = dnums.batch_dimension();
    const int64 z_dim = dnums.feature_dimension();
    // Dimension number applicable for kernel (rhs).
    const int64 kernel_input_z_dim = dnums.kernel_input_feature_dimension();
    const int64 kernel_output_z_dim = dnums.kernel_output_feature_dimension();

    const int64 z_size = ShapeUtil::GetDimension(lhs->shape(), z_dim);

    std::vector<int64> window_dimension_sizes;
    for (auto i : dnums.kernel_spatial_dimensions()) {
      window_dimension_sizes.push_back(
          ShapeUtil::GetDimension(rhs->shape(), i));
    }

    const Shape& window_shape = ShapeUtil::MakeShape(
        rhs->shape().element_type(), window_dimension_sizes);

    auto result = Literal::CreateFromShape(conv->shape());
    TF_RETURN_IF_ERROR(result->Populate<ReturnT>(
        [&](tensorflow::gtl::ArraySlice<int64> out_index) {
          ReturnT result_val = static_cast<ReturnT>(0);

          std::vector<int64> lhs_index(lhs_rank, 0);
          std::vector<int64> rhs_index(rhs_rank, 0);

          lhs_index[batch_dim] = out_index[batch_dim];
          rhs_index[kernel_output_z_dim] = out_index[z_dim];

          std::vector<int64> rhs_spatial_index(
              dnums.kernel_spatial_dimensions_size(), 0);

          // Convolve input feature with kernel.
          do {
            for (int64 iz = 0; iz < z_size; ++iz) {
              lhs_index[z_dim] = iz;
              rhs_index[kernel_input_z_dim] = iz;

              // Find corresponding spatial dimension index for input (lhs).
              for (int64 ki = 0; ki < rhs_spatial_index.size(); ++ki) {
                // Spatial dimension number for input (lhs) and output.
                const int64 spatial_dim = dnums.spatial_dimensions(ki);

                // Calculate lhs (input) index without taking base dilation into
                // account.
                const int64 undilated_index =
                    out_index[spatial_dim] * window.dimensions(ki).stride() -
                    window.dimensions(ki).padding_low() +
                    rhs_spatial_index[ki] *
                        window.dimensions(ki).window_dilation();
                // Skip if the lhs (input) index is to be dilated.
                if (undilated_index % window.dimensions(ki).base_dilation() !=
                    0) {
                  goto cnt;
                }

                // Calculate the actual lhs (input) index after dilation.
                lhs_index[spatial_dim] =
                    undilated_index / window.dimensions(ki).base_dilation();

                // Skip if input index is not in bound.
                if (!(lhs_index[spatial_dim] >= 0 &&
                      lhs_index[spatial_dim] <
                          lhs->shape().dimensions(spatial_dim))) {
                  goto cnt;
                }

                rhs_index[dnums.kernel_spatial_dimensions(ki)] =
                    rhs_spatial_index[ki];
              }

              result_val += lhs_literal.Get<ReturnT>(lhs_index) *
                            rhs_literal.Get<ReturnT>(rhs_index);
            }
          cnt:;
          } while (IndexUtil::BumpIndices(window_shape, &rhs_spatial_index));

          return result_val;
        }));

    parent_->evaluated_[conv] = std::move(result);
    return Status::OK();
  };

  Status HandleDot(HloInstruction* dot, HloInstruction* lhs,
                   HloInstruction* rhs) override {
    CHECK(ShapeUtil::IsArray(dot->shape()));
    CHECK(ShapeUtil::IsArray(lhs->shape()));
    CHECK(ShapeUtil::IsArray(rhs->shape()));

    // Dot only supports operands of rank 1 and 2.
    const auto dot_rank = ShapeUtil::Rank(dot->shape());
    const auto lhs_rank = ShapeUtil::Rank(lhs->shape());
    const auto rhs_rank = ShapeUtil::Rank(rhs->shape());
    CHECK(lhs_rank > 0 && lhs_rank <= 2);
    CHECK(rhs_rank > 0 && rhs_rank <= 2);
    CHECK_EQ(dot_rank, lhs_rank + rhs_rank - 2);

    CHECK(ShapeUtil::SameElementType(lhs->shape(), rhs->shape()));
    CHECK(ShapeUtil::SameElementType(lhs->shape(), dot->shape()));

    // Check contracted dimensions are the same.
    //
    // Determine the index of the contracted dimensions for input tensors.
    // dimensions -1 of lhs and dimension 0 of rhs are contracted.
    const int64 lhs_contracted_dimension =
        ShapeUtil::GetDimensionNumber(lhs->shape(), -1);
    const int64 rhs_contracted_dimension = 0;
    CHECK_EQ(lhs->shape().dimensions(lhs_contracted_dimension),
             rhs->shape().dimensions(rhs_contracted_dimension))
        << "lhs contracted dimension: "
        << lhs->shape().dimensions(lhs_contracted_dimension)
        << " rhs contracted dimension: "
        << rhs->shape().dimensions(rhs_contracted_dimension);
    const int64 contracted_dimension_size =
        lhs->shape().dimensions(lhs_contracted_dimension);

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);

    auto result = Literal::CreateFromShape(dot->shape());
    TF_RETURN_IF_ERROR(result->Populate<ReturnT>(
        [&](tensorflow::gtl::ArraySlice<int64> multi_index) {
          ReturnT result_val = static_cast<ReturnT>(0);

          std::vector<int64> lhs_index(lhs_rank, 0);
          std::vector<int64> rhs_index(rhs_rank, 0);
          // Set index for non-contracted dimension for lhs and rhs.
          if (lhs_rank > 1) {
            lhs_index[0] = multi_index[0];
          }
          if (rhs_rank > 1) {
            rhs_index[1] = multi_index[multi_index.size() - 1];
          }

          // Accumulates resulting product along the contracted dimension.
          for (int64 i = 0; i < contracted_dimension_size; ++i) {
            lhs_index[lhs_contracted_dimension] = i;
            rhs_index[rhs_contracted_dimension] = i;

            result_val += lhs_literal.Get<ReturnT>(lhs_index) *
                          rhs_literal.Get<ReturnT>(rhs_index);
          }

          return result_val;
        }));

    parent_->evaluated_[dot] = std::move(result);
    return Status::OK();
  };

  Status HandlePad(HloInstruction* pad) override {
    CHECK(!ShapeUtil::IsTuple(pad->operand(0)->shape()));
    // Padding value must be scalar.
    CHECK(ShapeUtil::IsScalar(pad->operand(1)->shape()));
    CHECK_EQ(ShapeUtil::Rank(pad->operand(0)->shape()),
             pad->padding_config().dimensions_size());

    TF_ASSIGN_OR_RETURN(auto inferred_return_shape,
                        ShapeInference::InferPadShape(
                            /*operand_shape=*/pad->operand(0)->shape(),
                            /*padding_value_shape=*/pad->operand(1)->shape(),
                            /*padding_config=*/pad->padding_config()));
    CHECK(ShapeUtil::Compatible(pad->shape(), inferred_return_shape))
        << "return shape is set to: " << ShapeUtil::HumanString(pad->shape())
        << "but is inferred to be: "
        << ShapeUtil::HumanString(inferred_return_shape);

    // Create new HLO of padded shape with padding value.
    ReturnT scalar =
        parent_->GetEvaluatedLiteralFor(pad->operand(1)).Get<ReturnT>({});
    auto result = Literal::CreateFromShape(pad->shape());
    TF_RETURN_IF_ERROR(result->Populate<ReturnT>(
        [&scalar](tensorflow::gtl::ArraySlice<int64> multi_index) {
          return scalar;
        }));

    auto evaluated_operand = parent_->GetEvaluatedLiteralFor(pad->operand(0));

    std::vector<int64> input_index(ShapeUtil::Rank(evaluated_operand.shape()),
                                   0);
    std::vector<int64> target_index(ShapeUtil::Rank(result->shape()), 0);

    // Loop through each element of the operand, assign them to the
    // corresponding index of the resulting padded literal.
    const PaddingConfig& pad_config = pad->padding_config();

    auto func = [&](const std::vector<int64>& input_index) {
      for (auto i = 0; i < input_index.size(); ++i) {
        // Interior padding occurs logically before edge padding, so in the case
        // of negative edge padding elements are removed from the
        // interior-padded operand.
        target_index[i] =
            pad_config.dimensions(i).edge_padding_low() +
            input_index[i] * (pad_config.dimensions(i).interior_padding() + 1);

        // Account for negative low and high padding: skip assignment if the
        // any target index is out of range.
        if (!(target_index[i] >= 0 &&
              target_index[i] < pad->shape().dimensions(i))) {
          return true;
        }
      }
      result->Set<ReturnT>(target_index,
                           evaluated_operand.Get<ReturnT>(input_index));
      return true;
    };

    std::vector<int64> zero_base(evaluated_operand.shape().dimensions_size(),
                                 0);
    std::vector<int64> step(evaluated_operand.shape().dimensions_size(), 1);

    ShapeUtil::ForEachIndex(
        evaluated_operand.shape(), zero_base,
        AsInt64Slice(evaluated_operand.shape().dimensions()), step, func);

    parent_->evaluated_[pad] = std::move(result);
    return Status::OK();
  };

  Status Preprocess(HloInstruction* hlo) override {
    VLOG(2) << hlo->ToString();
    return Status::OK();
  };

 private:
  StatusOr<std::unique_ptr<Literal>> ElementWiseUnaryOp(
      HloInstruction* instruction,
      const std::function<ReturnT(ReturnT)>& unary_op) {
    const Literal& operand_literal =
        parent_->GetEvaluatedLiteralFor(instruction->operand(0));
    return ElementWiseUnaryOpImpl<ReturnT, ReturnT>(instruction, unary_op,
                                                    operand_literal);
  }

  StatusOr<std::unique_ptr<Literal>> ElementWiseBinaryOp(
      HloInstruction* instruction,
      const std::function<ReturnT(ReturnT, ReturnT)>& binary_op) {
    const auto shape = instruction->shape();
    const auto* lhs = instruction->operand(0);
    const auto* rhs = instruction->operand(1);

    // TODO(b/35950897, b/27796129): add DCHECK back once implicit broadcast is
    // removed.
    if (!(ShapeUtil::SameDimensions(shape, rhs->shape()) &&
          ShapeUtil::SameDimensions(lhs->shape(), rhs->shape()))) {
      return Unimplemented(
          "Implicit broadcasting is currently unsupported in HLO evaluator "
          "Shape Mismatch: %s vs %s vs %s: ",
          ShapeUtil::HumanString(shape).c_str(),
          ShapeUtil::HumanString(lhs->shape()).c_str(),
          ShapeUtil::HumanString(rhs->shape()).c_str());
    }

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);

    auto result = Literal::CreateFromShape(shape);

    TF_RETURN_IF_ERROR(result->Populate<ReturnT>(
        [&](tensorflow::gtl::ArraySlice<int64> multi_index) {
          return binary_op(lhs_literal.Get<ReturnT>(multi_index),
                           rhs_literal.Get<ReturnT>(multi_index));
        }));
    return std::move(result);
  }

  template <typename LhsType, typename RhsType, typename EhsType>
  StatusOr<std::unique_ptr<Literal>> ElementWiseTernaryOp(
      HloInstruction* instruction,
      const std::function<ReturnT(LhsType, RhsType, EhsType)>& ternary_op) {
    const auto shape = instruction->shape();
    const auto* lhs = instruction->operand(0);
    const auto* rhs = instruction->operand(1);
    const auto* ehs = instruction->operand(2);

    // TODO(b/35950897, b/27796129): add DCHECK back once implicit broadcast is
    // removed.
    if (!(ShapeUtil::SameDimensions(shape, lhs->shape()) &&
          ShapeUtil::SameDimensions(lhs->shape(), rhs->shape()) &&
          ShapeUtil::SameDimensions(rhs->shape(), ehs->shape()))) {
      return Unimplemented(
          "Implicit broadcasting is currently unsupported in HLO evaluator "
          "Shape Mismatch: %s vs %s vs %s vs %s: ",
          ShapeUtil::HumanString(shape).c_str(),
          ShapeUtil::HumanString(lhs->shape()).c_str(),
          ShapeUtil::HumanString(rhs->shape()).c_str(),
          ShapeUtil::HumanString(ehs->shape()).c_str());
    }

    const Literal& lhs_literal = parent_->GetEvaluatedLiteralFor(lhs);
    const Literal& rhs_literal = parent_->GetEvaluatedLiteralFor(rhs);
    const Literal& ehs_literal = parent_->GetEvaluatedLiteralFor(ehs);

    auto result = Literal::CreateFromShape(shape);

    TF_RETURN_IF_ERROR(result->Populate<ReturnT>(
        [&](tensorflow::gtl::ArraySlice<int64> multi_index) {
          return ternary_op(lhs_literal.Get<LhsType>(multi_index),
                            rhs_literal.Get<RhsType>(multi_index),
                            ehs_literal.Get<EhsType>(multi_index));
        }));

    return std::move(result);
  }

  HloEvaluator* parent_;
};

HloEvaluator::HloEvaluator() {
  typed_visitors_[PRED] = MakeUnique<TypedVisitor<bool>>(this);
  typed_visitors_[U8] = MakeUnique<TypedVisitor<uint8>>(this);
  typed_visitors_[U16] = MakeUnique<FunctionVisitor>([](HloInstruction*) {
    return Unimplemented("unhandled primitive type: U16.");
  });
  typed_visitors_[U32] = MakeUnique<TypedVisitor<uint32>>(this);
  typed_visitors_[U64] = MakeUnique<TypedVisitor<uint64>>(this);
  typed_visitors_[S8] = MakeUnique<TypedVisitor<int8>>(this);
  typed_visitors_[S16] = MakeUnique<FunctionVisitor>([](HloInstruction*) {
    return Unimplemented("unhandled primitive type: S16.");
  });
  typed_visitors_[S32] = MakeUnique<TypedVisitor<int32>>(this);
  typed_visitors_[S64] = MakeUnique<TypedVisitor<int64>>(this);
  typed_visitors_[F16] = MakeUnique<FunctionVisitor>([](HloInstruction*) {
    return Unimplemented("unhandled primitive type: F16.");
  });
  typed_visitors_[F32] = MakeUnique<TypedVisitor<float>>(this);
  typed_visitors_[F64] = MakeUnique<TypedVisitor<double>>(this);
  typed_visitors_[TUPLE] = MakeUnique<FunctionVisitor>([](HloInstruction*) {
    return Unimplemented("unhandled primitive type: TUPLE.");
  });
  typed_visitors_[OPAQUE] = MakeUnique<FunctionVisitor>([](HloInstruction*) {
    return Unimplemented("unhandled primitive type: OPAQUE.");
  });
}

StatusOr<std::unique_ptr<Literal>> HloEvaluator::Evaluate(
    HloComputation* computation,
    tensorflow::gtl::ArraySlice<const Literal*> args) {
  arg_literals_ = args;
  evaluated_.clear();

  TF_RETURN_IF_ERROR(computation->Accept(this));
  return MakeUnique<Literal>(
      GetEvaluatedLiteralFor(computation->root_instruction()));
}

StatusOr<std::unique_ptr<Literal>> HloEvaluator::Evaluate(
    HloInstruction* instruction,
    tensorflow::gtl::ArraySlice<const Literal*> operands) {
  TF_RET_CHECK(hlo_query::AllOperandsAreParametersOrConstants(*instruction));
  TF_RETURN_IF_ERROR(ShapeUtil::ValidateShape(instruction->shape()));

  arg_literals_ = operands;
  evaluated_.clear();

  // Evaluate operands of Parameter type against the input literals which
  // caches the evaluated literal results.
  for (const auto operand : instruction->operands()) {
    if (operand->opcode() == HloOpcode::kParameter) {
      const Literal* input_literal = arg_literals_[operand->parameter_number()];
      VLOG(2) << "Parameter operand evaluated to: "
              << input_literal->ToString();
      TF_RET_CHECK(ShapeUtil::Equal(operand->shape(), input_literal->shape()));

      evaluated_[operand] = MakeUnique<Literal>(*input_literal);
    }
  }

  TF_RETURN_IF_ERROR(instruction->Visit(this));
  return MakeUnique<Literal>(GetEvaluatedLiteralFor(instruction));
}

StatusOr<std::unique_ptr<Literal>> HloEvaluator::Evaluate(
    HloInstruction* instruction) {
  TF_RET_CHECK(hlo_query::AllOperandsAreConstants(*instruction));
  TF_RET_CHECK(instruction->opcode() != HloOpcode::kParameter);
  TF_RETURN_IF_ERROR(ShapeUtil::ValidateShape(instruction->shape()));

  arg_literals_.clear();
  evaluated_.clear();
  TF_RETURN_IF_ERROR(instruction->Visit(this));
  return MakeUnique<Literal>(GetEvaluatedLiteralFor(instruction));
}

std::unique_ptr<Literal> HloEvaluator::TryEvaluate(
    HloInstruction* instruction) {
  auto result_or = Evaluate(instruction);
  if (!result_or.ok()) {
    VLOG(1) << "TryEvaluate failed:" << result_or.status();
    return nullptr;
  }

  return result_or.ConsumeValueOrDie();
}

Status HloEvaluator::HandleParameter(HloInstruction* parameter) {
  VLOG(2) << "HandleParameter: " << parameter->ToString();
  const Literal* input_literal = arg_literals_[parameter->parameter_number()];
  VLOG(2) << "Parameter evaluated to: " << input_literal->ToString();
  DCHECK(ShapeUtil::Equal(parameter->shape(), input_literal->shape()));

  evaluated_[parameter] = MakeUnique<Literal>(*input_literal);
  return Status::OK();
}

Status HloEvaluator::HandleConstant(HloInstruction* constant,
                                    const Literal& literal) {
  VLOG(2) << "HandleConstant: " << constant->ToString();
  return Status::OK();
}

Status HloEvaluator::HandleReshape(HloInstruction* reshape) {
  TF_ASSIGN_OR_RETURN(
      evaluated_[reshape],
      GetEvaluatedLiteralFor(reshape->operand(0))
          .Reshape(AsInt64Slice(reshape->shape().dimensions())));
  return Status::OK();
}

Status HloEvaluator::HandleTranspose(HloInstruction* transpose) {
  evaluated_[transpose] = GetEvaluatedLiteralFor(transpose->operand(0))
                              .Transpose(transpose->dimensions());
  return Status::OK();
}

Status HloEvaluator::HandleConcatenate(
    HloInstruction* concatenate,
    tensorflow::gtl::ArraySlice<HloInstruction*> operands) {
  // The result concatenate dimension is going to be the sum of all concatenate
  // dimensions of the operands taking part of the operation.
  const Shape& reference_shape = operands[0]->shape();
  CHECK(!ShapeUtil::IsTuple(reference_shape));
  const int64 rank = ShapeUtil::Rank(reference_shape);
  const int64 concat_dim = concatenate->dimensions()[0];
  CHECK_GE(concat_dim, 0);
  CHECK_LT(concat_dim, rank);

  DimensionVector concat_dimensions(reference_shape.dimensions().begin(),
                                    reference_shape.dimensions().end());

  for (int64 i = 1; i < operands.size(); ++i) {
    const Shape& operand_shape = operands[i]->shape();
    CHECK(!ShapeUtil::IsTuple(operand_shape));
    // Accumulate the concat dimension from all tensors taking part to the
    // operation.
    concat_dimensions[concat_dim] +=
        ShapeUtil::GetDimension(operand_shape, concat_dim);
  }

  auto result_literal = Literal::CreateFromDimensions(
      reference_shape.element_type(), concat_dimensions);
  DimensionVector source_indices(rank, 0);
  DimensionVector dest_indices(concat_dimensions.size(), 0);

  for (auto operand : operands) {
    const Shape& operand_shape = operand->shape();
    TF_RETURN_IF_ERROR(result_literal->Copy(
        GetEvaluatedLiteralFor(operand), source_indices, dest_indices,
        AsInt64Slice(operand_shape.dimensions())));
    dest_indices[concat_dim] +=
        ShapeUtil::GetDimension(operand_shape, concat_dim);
  }

  evaluated_[concatenate] = std::move(result_literal);
  return Status::OK();
}

Status HloEvaluator::HandleIsFinite(HloInstruction* is_finite,
                                    HloInstruction* operand) {
  if (!ShapeUtil::ElementIsFloating(operand->shape())) {
    return InvalidArgument(
        "expected element type in shape to be float for IsFinite op, got: %s",
        PrimitiveType_Name(operand->shape().element_type()).c_str());
  }

  switch (operand->shape().element_type()) {
    case F16:
      return Unimplemented("unhandled primitive type: F16.");
    case F32: {
      auto result_or = ElementWiseUnaryOpImpl<bool, float>(
          is_finite,
          [](float elem_operand) { return std::isfinite(elem_operand); },
          GetEvaluatedLiteralFor(operand));
      TF_ASSIGN_OR_RETURN(evaluated_[is_finite], std::move(result_or));
      break;
    }
    case F64: {
      auto result_or = ElementWiseUnaryOpImpl<bool, double>(
          is_finite,
          [](double elem_operand) { return std::isfinite(elem_operand); },
          GetEvaluatedLiteralFor(operand));
      TF_ASSIGN_OR_RETURN(evaluated_[is_finite], std::move(result_or));
      break;
    }
    default:
      LOG(FATAL) << "unknown/unhandled primitive type.";
  }

  return Status::OK();
}

Status HloEvaluator::HandleCompare(HloInstruction* compare, HloOpcode opcode,
                                   HloInstruction* lhs, HloInstruction* rhs) {
  // TODO(b/35950897, b/27796129): add DCHECK back once implicit broadcast is
  // removed.
  if (!(ShapeUtil::SameDimensions(compare->shape(), rhs->shape()) &&
        ShapeUtil::SameDimensions(lhs->shape(), rhs->shape()))) {
    return Unimplemented(
        "Implicit broadcasting is currently unsupported in HLO evaluator "
        "Shape Mismatch: %s vs %s vs %s",
        ShapeUtil::HumanString(compare->shape()).c_str(),
        ShapeUtil::HumanString(lhs->shape()).c_str(),
        ShapeUtil::HumanString(rhs->shape()).c_str());
  }

  TF_RET_CHECK(lhs->shape().element_type() == rhs->shape().element_type());

  const Literal& lhs_literal = GetEvaluatedLiteralFor(lhs);
  const Literal& rhs_literal = GetEvaluatedLiteralFor(rhs);

  // Note here we switch on the operand's type.
  switch (lhs->shape().element_type()) {
    case PRED: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<bool>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case U8: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<uint8>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case U16:
      return Unimplemented("unhandled primitive type: U16.");
    case U32: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<uint32>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case U64: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<uint64>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case S8: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<int8>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case S16:
      return Unimplemented("unhandled primitive type: S16.");
    case S32: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<int32>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case S64: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<int64>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case F16:
      return Unimplemented("unhandled primitive type: F16.");
    case F32: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<float>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    case F64: {
      TF_ASSIGN_OR_RETURN(
          evaluated_[compare],
          Compare<double>(compare->shape(), opcode, lhs_literal, rhs_literal));
    } break;
    default:
      LOG(FATAL) << "unknown primitive type.";
  }

  return Status::OK();
}

Status HloEvaluator::HandleSlice(HloInstruction* slice,
                                 HloInstruction* operand) {
  const Shape& shape = slice->shape();
  auto literal = Literal::CreateFromDimensions(
      shape.element_type(), AsInt64Slice(shape.dimensions()));

  DimensionVector dest_indices(slice->slice_starts().size(), 0);

  TF_RETURN_IF_ERROR(literal->Copy(GetEvaluatedLiteralFor(operand),
                                   slice->slice_starts(), dest_indices,
                                   AsInt64Slice(shape.dimensions())));

  evaluated_[slice] = std::move(literal);
  return Status::OK();
}

}  // namespace xla
