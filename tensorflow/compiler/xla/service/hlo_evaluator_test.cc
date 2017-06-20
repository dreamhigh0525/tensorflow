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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/types.h"

namespace xla {
namespace {

class HloEvaluatorTest : public HloTestBase {
 protected:
  HloEvaluatorTest() { evaluator_ = MakeUnique<HloEvaluator>(); }

  std::unique_ptr<HloEvaluator> evaluator_;
};

// Verifies that HloEvaluator evaluates a HLO instruction that performs clamp
// with 3 operands.
TEST_F(HloEvaluatorTest, DoesClamp) {
  auto low = Literal::CreateR2<float>({{0.f, 2.f}, {2.f, 4.f}});
  auto high = Literal::CreateR2<float>({{2.f, 4.f}, {4.f, 4.f}});
  auto value = Literal::CreateR2<float>({{0.f, 5.f}, {0.f, 4.f}});

  Shape shape = low->shape();
  auto c1 = HloInstruction::CreateConstant(std::move(low));
  auto c2 = HloInstruction::CreateConstant(std::move(high));
  auto c3 = HloInstruction::CreateConstant(std::move(value));
  auto instruction = HloInstruction::CreateTernary(
      shape, HloOpcode::kClamp, c1.get(), c2.get(), c3.get());

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(instruction.get(), {}).ConsumeValueOrDie();

  auto expected = Literal::CreateR2<float>({{0, 4}, {2, 4}});

  EXPECT_TRUE(result->Equal(*expected));
}

// Verifies that HloEvaluator evaluates a HLO instruction that performs select
// with 3 operands.
TEST_F(HloEvaluatorTest, DoesSelect) {
  auto pred = Literal::CreateR2<bool>({{true, false}, {false, true}});
  auto on_true = Literal::CreateR2<float>({{2.f, 4.f}, {4.f, 4.f}});
  auto on_false = Literal::CreateR2<float>({{0.f, 5.f}, {0.f, 4.f}});

  Shape shape = on_true->shape();
  auto c1 = HloInstruction::CreateConstant(std::move(pred));
  auto c2 = HloInstruction::CreateConstant(std::move(on_true));
  auto c3 = HloInstruction::CreateConstant(std::move(on_false));
  auto instruction = HloInstruction::CreateTernary(
      shape, HloOpcode::kSelect, c1.get(), c2.get(), c3.get());

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(instruction.get(), {}).ConsumeValueOrDie();

  auto expected = Literal::CreateR2<float>({{2, 5}, {0, 4}});

  EXPECT_TRUE(result->Equal(*expected));
}

// Verifies that HloEvaluator evaluates a HLO instruction that performs
// element-wise addition with 2 operands.
TEST_F(HloEvaluatorTest, DoesAdd) {
  auto lhs = Literal::CreateR2<int64>({{1, 0}, {-100, 4}});
  auto rhs = Literal::CreateR2<int64>({{2, 4}, {4, 4}});

  Shape shape = ShapeUtil::MakeShape(S64, {2, 2});
  auto c1 = HloInstruction::CreateConstant(std::move(lhs));
  auto c2 = HloInstruction::CreateConstant(std::move(rhs));
  auto instruction =
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, c1.get(), c2.get());

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(instruction.get(), {}).ConsumeValueOrDie();

  auto expected = Literal::CreateR2<int64>({{3, 4}, {-96, 8}});

  EXPECT_TRUE(result->Equal(*expected));
}

// Verifies that HloEvaluator evaluates a HLO instruction that performs
// element-wise divide with 2 operands.
TEST_F(HloEvaluatorTest, DoesDivide) {
  auto lhs_s64 = Literal::CreateR2<int64>({{1, 0}, {-100, 4}});
  auto rhs_s64 = Literal::CreateR2<int64>({{2, 4}, {4, 4}});

  Shape shape_s64 = ShapeUtil::MakeShape(S64, {2, 2});
  auto c1_s64 = HloInstruction::CreateConstant(std::move(lhs_s64));
  auto c2_s64 = HloInstruction::CreateConstant(std::move(rhs_s64));
  auto instruction = HloInstruction::CreateBinary(shape_s64, HloOpcode::kDivide,
                                                  c1_s64.get(), c2_s64.get());

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(instruction.get(), {}).ConsumeValueOrDie();

  auto expected = Literal::CreateR2<int64>({{0, 0}, {-25, 1}});

  EXPECT_TRUE(result->Equal(*expected));

  auto lhs_f64 = Literal::CreateR2<double>({{1.0, 0.0}, {-100.0, 4.0}});
  auto rhs_f64 = Literal::CreateR2<double>({{2.2, 4.0}, {4.0, 4.0}});

  Shape shape_f64 = ShapeUtil::MakeShape(F64, {2, 2});
  auto c1_f64 = HloInstruction::CreateConstant(std::move(lhs_f64));
  auto c2_f64 = HloInstruction::CreateConstant(std::move(rhs_f64));
  instruction = HloInstruction::CreateBinary(shape_f64, HloOpcode::kDivide,
                                             c1_f64.get(), c2_f64.get());

  result = evaluator_->Evaluate(instruction.get(), {}).ConsumeValueOrDie();

  expected = Literal::CreateR2<double>({{0.45454545454545453, 0}, {-25, 1}});

  EXPECT_TRUE(result->Equal(*expected));
}

// Verifies that HloEvaluator evaluates a HLO instruction that performs
// element-wise abs op with 1 operand.
TEST_F(HloEvaluatorTest, DoesAbs) {
  auto operand = Literal::CreateR2<int64>({{1, -20}, {-100, 4}});
  const Shape& shape = ShapeUtil::MakeShape(S64, {2, 2});
  auto c1 = HloInstruction::CreateConstant(std::move(operand));
  auto instruction =
      HloInstruction::CreateUnary(shape, HloOpcode::kAbs, c1.get());

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(instruction.get(), {}).ConsumeValueOrDie();

  auto expected = Literal::CreateR2<int64>({{1, 20}, {100, 4}});

  EXPECT_TRUE(result->Equal(*expected));

  // For R0 literal.
  const Shape& r0 = ShapeUtil::MakeShape(F32, {});
  operand = Literal::CreateR0<float>(-1.0f);
  c1 = HloInstruction::CreateConstant(std::move(operand));
  instruction = HloInstruction::CreateUnary(r0, HloOpcode::kAbs, c1.get());
  result = evaluator_->Evaluate(instruction.get()).ConsumeValueOrDie();
  expected = Literal::CreateR0<float>(1.0f);

  EXPECT_TRUE(result->Equal(*expected));

  // For R1 literal with dimension of size 0.
  Shape empty_r1 = ShapeUtil::MakeShape(F32, {0});
  operand = Literal::CreateR1<float>({});
  c1 = HloInstruction::CreateConstant(std::move(operand));
  instruction =
      HloInstruction::CreateUnary(empty_r1, HloOpcode::kAbs, c1.get());

  result = evaluator_->Evaluate(instruction.get()).ConsumeValueOrDie();
  expected = Literal::CreateR1<float>({});

  EXPECT_TRUE(result->Equal(*expected));
}  // namespace

// Verifies that HloEvaluator evaluates a HLO Computation with non-parameter nor
// constant operands.
TEST_F(HloEvaluatorTest, DoesTraverseInstructions) {
  HloComputation::Builder builder(TestName());
  auto lhs = Literal::CreateR2<int64>({{1, 0}, {-100, 4}});
  auto rhs = Literal::CreateR2<int64>({{2, 4}, {4, 4}});
  auto rhs2 = Literal::CreateR2<int64>({{1, -20}, {-100, 4}});
  std::vector<const Literal*> args = {lhs.get(), rhs.get(), rhs2.get()};

  Shape shape = ShapeUtil::MakeShape(S64, {2, 2});

  auto param_lhs = HloInstruction::CreateParameter(0, shape, "lhs");
  auto param_rhs = HloInstruction::CreateParameter(1, shape, "rhs");
  auto lhs_instruction = HloInstruction::CreateBinary(
      shape, HloOpcode::kAdd, param_lhs.get(), param_rhs.get());

  auto param_rhs2 = HloInstruction::CreateParameter(2, shape, "rhs2");
  auto root_instruction = HloInstruction::CreateBinary(
      shape, HloOpcode::kAdd, lhs_instruction.get(), param_rhs2.get());

  builder.AddInstruction(std::move(root_instruction));
  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(builder.Build().get(), args).ConsumeValueOrDie();

  auto expected = Literal::CreateR2<int64>({{4, -16}, {-196, 12}});

  EXPECT_TRUE(result->Equal(*expected));
}

// Verifies Reshape operation is correctly evaluated.
TEST_F(HloEvaluatorTest, DoesReshape) {
  HloComputation::Builder builder(TestName());
  const int64 dimensions[] = {11, 8, 7, 5, 9};
  TF_ASSIGN_OR_ASSERT_OK(auto literal,
                         LiteralTestUtil::CreateRandomLiteral<F32>(
                             ShapeUtil::MakeShape(F32, dimensions), 0.0, 1.0));
  auto literal_clone = literal->CloneToUnique();
  HloInstruction* literal_instruction = builder.AddInstruction(
      HloInstruction::CreateConstant(std::move(literal)));

  Shape shape = ShapeUtil::MakeShape(F32, {8, 7, 11, 9, 5});
  const int64 permutation[] = {1, 2, 0, 4, 3};
  builder.AddInstruction(
      HloInstruction::CreateTranspose(shape, literal_instruction, permutation));

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(builder.Build().get(), {}).ConsumeValueOrDie();

  using NativeT = typename primitive_util::PrimitiveTypeToNative<F32>::type;
  result->EachCell<NativeT>(
      [&](tensorflow::gtl::ArraySlice<int64> indices, NativeT value) {
        std::vector<int64> rindexes = Permute(permutation, indices);
        EXPECT_TRUE(value == literal_clone->Get<NativeT>(rindexes));
      });
}

// Verifies Broadcast operation is correctly evaluated.
TEST_F(HloEvaluatorTest, DoesBroadcast) {
  HloComputation::Builder builder(TestName());
  auto input_literal = Literal::CreateR2<int32>({{1, 2}, {3, 4}, {5, 6}});
  auto output_literal = Literal::CreateR3<int32>(
      {{{1, 2}, {3, 4}, {5, 6}}, {{1, 2}, {3, 4}, {5, 6}}});
  HloInstruction* literal_instruction = builder.AddInstruction(
      HloInstruction::CreateConstant(std::move(input_literal)));

  builder.AddInstruction(HloInstruction::CreateBroadcast(
      output_literal->shape(), literal_instruction, {1, 2}));

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(builder.Build().get(), {}).ConsumeValueOrDie();

  LiteralTestUtil::ExpectEqual(*result, *output_literal);
}

TEST_F(HloEvaluatorTest, ConvertWithSameLayout) {
  HloComputation::Builder builder(TestName());

  auto input_literal = Literal::CreateR2<int32>({{1, 2}, {3, 4}, {5, 6}});
  auto expected =
      Literal::CreateR2<float>({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}});
  ASSERT_TRUE(LayoutUtil::LayoutsInShapesEqual(input_literal->shape(),
                                               expected->shape()));

  HloInstruction* constant = builder.AddInstruction(
      HloInstruction::CreateConstant(std::move(input_literal)));
  builder.AddInstruction(
      HloInstruction::CreateConvert(expected->shape(), constant));

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(builder.Build().get(), {}).ConsumeValueOrDie();

  EXPECT_TRUE(ShapeUtil::Equal(result->shape(), expected->shape()));
  LiteralTestUtil::ExpectEqual(*result, *expected);
}

TEST_F(HloEvaluatorTest, ConvertWithDifferentLayout) {
  HloComputation::Builder builder(TestName());

  auto input_literal = Literal::CreateR2WithLayout<int32>(
      {{1, 2}, {3, 4}, {5, 6}}, LayoutUtil::MakeLayout({0, 1}));
  auto expected = Literal::CreateR2WithLayout<float>(
      {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}}, LayoutUtil::MakeLayout({1, 0}));
  ASSERT_FALSE(LayoutUtil::LayoutsInShapesEqual(input_literal->shape(),
                                                expected->shape()));

  HloInstruction* constant = builder.AddInstruction(
      HloInstruction::CreateConstant(std::move(input_literal)));
  builder.AddInstruction(
      HloInstruction::CreateConvert(expected->shape(), constant));

  std::unique_ptr<Literal> result =
      evaluator_->Evaluate(builder.Build().get(), {}).ConsumeValueOrDie();

  EXPECT_TRUE(ShapeUtil::Equal(result->shape(), expected->shape()));
  LiteralTestUtil::ExpectEqual(*result, *expected);
}

}  // namespace
}  // namespace xla
