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

#include "tensorflow/compiler/xla/service/dynamic_padder.h"

#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_matchers.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/service/hlo_runner.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/test_helpers.h"
#include "tensorflow/compiler/xla/tests/client_library_test_base.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test_benchmark.h"

namespace op = xla::testing::opcode_matchers;

namespace xla {
namespace {

class DynamicPadderTest : public HloTestBase {
 protected:
  DynamicPadderTest() : HloTestBase() { module_ = CreateNewVerifiedModule(); }

  StatusOr<bool> RunPadder() {
    DynamicPadder padder;
    return padder.Run(module_.get());
  }

  void ExpectPadded(const HloInstruction* inst) {
    EXPECT_THAT(inst,
                op::Select(op::Lt(op::Iota(), op::Broadcast(op::Parameter())),
                           ::testing::_, op::Broadcast()));
  }

  HloComputation* GetScalarAddComputation() {
    auto embedded_builder = HloComputation::Builder("add");
    auto lhs = embedded_builder.AddInstruction(HloInstruction::CreateParameter(
        0, ShapeUtil::MakeShape(F32, {}), "lhs"));
    auto rhs = embedded_builder.AddInstruction(HloInstruction::CreateParameter(
        1, ShapeUtil::MakeShape(F32, {}), "rhs"));
    embedded_builder.AddInstruction(
        HloInstruction::CreateBinary(lhs->shape(), HloOpcode::kAdd, lhs, rhs));
    return module_->AddEmbeddedComputation(embedded_builder.Build());
  }

  std::unique_ptr<HloModule> module_;
  const Shape scalar_shape_ = ShapeUtil::MakeShape(S32, {});
};

TEST_F(DynamicPadderTest, ReduceTest) {
  auto builder = HloComputation::Builder(TestName());
  auto input_shape = ShapeUtil::MakeShape(F32, {1, 2, 2});
  auto reduce_shape = ShapeUtil::MakeShape(F32, {2});

  auto data_param = builder.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "data_param"));
  builder.AddInstruction(
      HloInstruction::CreateParameter(1, scalar_shape_, "size_param"));

  auto negate = builder.AddInstruction(
      HloInstruction::CreateUnary(input_shape, HloOpcode::kNegate, data_param));

  auto init = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(0.0)));

  auto reduce = builder.AddInstruction(HloInstruction::CreateReduce(
      reduce_shape, negate, init, {0, 2}, GetScalarAddComputation()));

  module_->AddEntryComputation(builder.Build());

  // Set up dynamic parameter binding.
  TF_CHECK_OK(module_->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{1, {}},
      DynamicParameterBinding::DynamicDimension{0, {}, 1}));

  TF_ASSERT_OK(RunPadder().status());

  ExpectPadded(reduce->operand(0));
}

TEST_F(DynamicPadderTest, ConvolutionTest) {
  auto builder = HloComputation::Builder(TestName());
  constexpr int xdim = 3;
  constexpr int ydim = 2;
  constexpr int zdim = 1;
  auto xy_shape = ShapeUtil::MakeShape(F32, {xdim, ydim});
  auto yz_shape = ShapeUtil::MakeShape(F32, {ydim, zdim});
  auto zx_shape = ShapeUtil::MakeShape(F32, {zdim, xdim});

  auto* a_param = builder.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/0, xy_shape, "A"));
  auto* b_param = builder.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/1, yz_shape, "B"));
  builder.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/2, scalar_shape_, "size_param"));

  auto dnums = XlaBuilder::CreateDefaultConvDimensionNumbers(0);

  dnums.set_kernel_input_feature_dimension(0);
  dnums.set_kernel_output_feature_dimension(1);
  dnums.set_input_batch_dimension(0);
  dnums.set_output_batch_dimension(1);
  dnums.set_output_feature_dimension(0);

  Window window;

  auto* conv = builder.AddInstruction(HloInstruction::CreateConvolve(
      zx_shape, a_param, b_param, /*feature_group_count=*/1,
      /*batch_group_count=*/1, window, dnums,
      HloTestBase::DefaultPrecisionConfig(2)));

  module_->AddEntryComputation(builder.Build());

  // Set up binding for contracting dimensions.
  TF_CHECK_OK(module_->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{2, {}},
      DynamicParameterBinding::DynamicDimension{0, {}, 1}));

  TF_ASSERT_OK(RunPadder().status());

  ExpectPadded(conv->operand(0));
}

TEST_F(DynamicPadderTest, ConvolutionNoPad) {
  auto builder = HloComputation::Builder(TestName());
  constexpr int xdim = 3;
  constexpr int ydim = 2;
  constexpr int zdim = 1;
  auto xy_shape = ShapeUtil::MakeShape(F32, {xdim, ydim});
  auto yz_shape = ShapeUtil::MakeShape(F32, {ydim, zdim});
  auto zx_shape = ShapeUtil::MakeShape(F32, {zdim, xdim});

  auto* a_param = builder.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/0, xy_shape, "A"));
  auto* b_param = builder.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/1, yz_shape, "B"));
  builder.AddInstruction(HloInstruction::CreateParameter(
      /*parameter_number=*/2, scalar_shape_, "size_param"));

  auto dnums = XlaBuilder::CreateDefaultConvDimensionNumbers(0);

  dnums.set_kernel_input_feature_dimension(0);
  dnums.set_kernel_output_feature_dimension(1);
  dnums.set_input_batch_dimension(0);
  dnums.set_output_batch_dimension(1);
  dnums.set_output_feature_dimension(0);

  Window window;

  auto* conv = builder.AddInstruction(HloInstruction::CreateConvolve(
      zx_shape, a_param, b_param, /*feature_group_count=*/1,
      /*batch_group_count=*/1, window, dnums,
      HloTestBase::DefaultPrecisionConfig(2)));

  module_->AddEntryComputation(builder.Build());

  // Set up dynamic parameter binding for non-contracting dimension.
  TF_CHECK_OK(module_->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{2, {}},
      DynamicParameterBinding::DynamicDimension{0, {}, 0}));

  TF_ASSERT_OK(RunPadder().status());

  EXPECT_THAT(conv->operand(0), op::Parameter());
}

TEST_F(DynamicPadderTest, ReduceWindowNoPadForTrivialWindow) {
  auto builder = HloComputation::Builder(TestName());
  auto input_shape = ShapeUtil::MakeShape(F32, {4, 5});
  auto reduce_shape = ShapeUtil::MakeShape(F32, {3, 5});

  auto input = builder.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  builder.AddInstruction(
      HloInstruction::CreateParameter(1, scalar_shape_, "size_param"));
  auto init = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(0.0)));
  TF_ASSERT_OK_AND_ASSIGN(Window window, ParseWindow("size=2x1 pad=0_0x0_0"));
  auto output = builder.AddInstruction(HloInstruction::CreateReduceWindow(
      reduce_shape, input, init, window, GetScalarAddComputation()));

  module_->AddEntryComputation(builder.Build());

  // Set up dynamic parameter binding.
  TF_CHECK_OK(module_->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{1, {}},
      DynamicParameterBinding::DynamicDimension{0, {}, 1}));

  TF_ASSERT_OK(RunPadder().status());

  EXPECT_THAT(output->operand(0), op::Parameter());
}

// Test that dynamic padder has the same result as if not padded.
class ExecutionTest : public HloTestBase {
 protected:
  std::unique_ptr<HloModule> GetHloModule(const string& hlo_text) {
    HloModuleConfig config;
    config.set_debug_options(GetDebugOptionsForTest());
    std::unique_ptr<HloModule> module =
        ParseAndReturnUnverifiedModule(hlo_text, config).ValueOrDie();
    return module;
  }
};

XLA_TEST_F(ExecutionTest, ScatterUpdate) {
  // Test that scattering on indices=[2] is same as scattering on indices=[4]
  // and dynamic dimension = 2
  const string hlo_text = R"(
HloModule TensorFlowScatterV1

update_s32 (lhs: s32[], rhs: s32[]) -> s32[] {
  lhs = s32[] parameter(0)
  ROOT rhs = s32[] parameter(1)
}

ENTRY main {
  operand = s32[3,3] parameter(0)
  indices = s32[INDICES_BOUND] parameter(1)
  updates = s32[INDICES_BOUND,3] parameter(2)
  dynamic_size = s32[] parameter(3)
  ROOT scatter = s32[3,3] scatter(operand, indices, updates),
      to_apply=update_s32,
      update_window_dims={1},
      inserted_window_dims={0},
      scatter_dims_to_operand_dims={0},
      index_vector_dim=1

}
)";
  const string hlo_text_not_padded =
      absl::StrReplaceAll(hlo_text, {{"INDICES_BOUND", "2"}});
  auto module_not_padded = GetHloModule(hlo_text_not_padded);

  Literal operand =
      LiteralUtil::CreateR2<int32>({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
  Literal scatter_indices = LiteralUtil::CreateR1<int32>({0, 2});
  Literal updates = LiteralUtil::CreateR2<int32>({{10, 20, 30}, {70, 80, 90}});
  Literal dynamic_size = LiteralUtil::CreateR0<int32>(2);

  Literal not_padded =
      ExecuteAndTransfer(std::move(module_not_padded),
                         {&operand, &scatter_indices, &updates, &dynamic_size});

  // Pad input to 4.
  const string hlo_text_padded =
      absl::StrReplaceAll(hlo_text, {{"INDICES_BOUND", "4"}});
  auto module_padded = GetHloModule(hlo_text_padded);
  // Set up dynamic parameter binding.
  TF_CHECK_OK(module_padded->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{3, {}},
      DynamicParameterBinding::DynamicDimension{1, {}, 0}));
  TF_CHECK_OK(module_padded->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{3, {}},
      DynamicParameterBinding::DynamicDimension{2, {}, 0}));
  // Pad the rest of input with garbage data.
  Literal scatter_indices_padded = LiteralUtil::CreateR1<int32>({0, 2, 0, 4});
  Literal updates_padded = LiteralUtil::CreateR2<int32>(
      {{10, 20, 30}, {70, 80, 90}, {30, 22, 11}, {-1, 20, -1}});
  DynamicPadder padder;
  TF_CHECK_OK(padder.Run(module_padded.get()).status());
  Literal padded = ExecuteAndTransfer(
      std::move(module_padded),
      {&operand, &scatter_indices_padded, &updates_padded, &dynamic_size});

  EXPECT_EQ(padded, not_padded);
}

XLA_TEST_F(ExecutionTest, TwoDimensionReduce) {
  // Test that reducing on operand=[2,2] is same as reducing on operand=[4,4]
  // and dynamic dimension = 2
  const string hlo_text = R"(
HloModule TensorFlowScatterV1

update_s32 (lhs: s32[], rhs: s32[]) -> s32[] {
  lhs = s32[] parameter(0)
  rhs = s32[] parameter(1)
  ROOT add = s32[] add(lhs, rhs)
}

ENTRY main {
  param = s32[INDICES_BOUND, INDICES_BOUND] parameter(0)
  dynamic_size = s32[] parameter(1)
  const = s32[] constant(0)
  ROOT reduce = s32[] reduce(param, const),
      dimensions={0, 1},
      to_apply=update_s32
}
)";
  const string hlo_text_not_padded =
      absl::StrReplaceAll(hlo_text, {{"INDICES_BOUND", "2"}});
  auto module_not_padded = GetHloModule(hlo_text_not_padded);

  Literal operand = LiteralUtil::CreateR2<int32>({{1, 2}, {4, 5}});
  Literal dynamic_size = LiteralUtil::CreateR0<int32>(2);

  Literal not_padded = ExecuteAndTransfer(std::move(module_not_padded),
                                          {&operand, &dynamic_size});

  // Pad input to 4.
  const string hlo_text_padded =
      absl::StrReplaceAll(hlo_text, {{"INDICES_BOUND", "4"}});
  auto module_padded = GetHloModule(hlo_text_padded);
  // Set up dynamic parameter binding.
  TF_CHECK_OK(module_padded->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{1, {}},
      DynamicParameterBinding::DynamicDimension{0, {}, 0}));
  TF_CHECK_OK(module_padded->dynamic_parameter_binding().Bind(
      DynamicParameterBinding::DynamicParameter{1, {}},
      DynamicParameterBinding::DynamicDimension{0, {}, 1}));
  // Pad the rest of input with garbage data.
  Literal operand_padded = LiteralUtil::CreateR2<int32>(
      {{1, 2, 3, 4}, {4, 5, 6, 7}, {1, 2, 3, 4}, {4, 5, 6, 7}});
  DynamicPadder padder;
  TF_CHECK_OK(padder.Run(module_padded.get()).status());
  Literal padded = ExecuteAndTransfer(std::move(module_padded),
                                      {&operand_padded, &dynamic_size});

  EXPECT_EQ(padded, not_padded);
}

}  // namespace
}  // namespace xla
