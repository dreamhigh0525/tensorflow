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

#include "tensorflow/compiler/xla/tools/hlo_tfgraph_builder.h"
#include "tensorflow/compiler/xla/client/computation_builder.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"

namespace xla {
namespace tools {
namespace {

using ::tensorflow::GraphDef;

class HloTfGraphBuilderTest : public HloTestBase {
 protected:
  HloTfGraphBuilderTest() {}
  HloTfGraphBuilder generator_;

  // Create a computation which takes a scalar and returns its negation.
  std::unique_ptr<HloComputation> CreateNegateComputation() {
    auto builder = HloComputation::Builder("Negate");
    auto param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, r0f32_, "param0"));
    builder.AddInstruction(
        HloInstruction::CreateUnary(r0f32_, HloOpcode::kNegate, param));
    return builder.Build();
  }

  // Creates a computation which calls map with the given computation.
  std::unique_ptr<HloComputation> CreateMapComputation(
      HloComputation* map_computation) {
    auto builder = HloComputation::Builder("Map");
    auto param = builder.AddInstruction(
        HloInstruction::CreateParameter(0, r0f32_, "param0"));
    builder.AddInstruction(
        HloInstruction::CreateMap(r0f32_, {param}, map_computation));
    return builder.Build();
  }
  Shape r0f32_ = ShapeUtil::MakeShape(F32, {});
};

TEST_F(HloTfGraphBuilderTest, SimpleNegateComputation) {
  auto negate_computation = CreateNegateComputation();
  TF_CHECK_OK(generator_.AddComputation(*negate_computation));
  GraphDef graph_def = generator_.GetGraphDef();
  EXPECT_EQ(graph_def.node_size(), 2);
  EXPECT_EQ(graph_def.node(0).name(), "Negate/param0.0");
  EXPECT_EQ(graph_def.node(0).op(), "HloParameter");
  EXPECT_EQ(graph_def.node(1).name(), "Negate/negate");
  EXPECT_EQ(graph_def.node(1).op(), "HloNegate");
  EXPECT_EQ(graph_def.node(1).input_size(), 1);
  EXPECT_EQ(graph_def.node(1).input(0), "Negate/param0.0");
}

TEST_F(HloTfGraphBuilderTest, GreaterThanOrEqualTo) {
  auto builder = HloComputation::Builder("GE");
  auto param_1 = builder.AddInstruction(
      HloInstruction::CreateParameter(0, r0f32_, "param0"));
  auto param_2 = builder.AddInstruction(
      HloInstruction::CreateParameter(1, r0f32_, "param1"));
  builder.AddInstruction(
      HloInstruction::CreateBinary(r0f32_, HloOpcode::kGe, param_1, param_2));
  TF_CHECK_OK(generator_.AddComputation(*builder.Build()));
  GraphDef graph_def = generator_.GetGraphDef();
  EXPECT_EQ(graph_def.node_size(), 3);
  EXPECT_EQ(graph_def.node(0).name(), "GE/param0.0");
  EXPECT_EQ(graph_def.node(1).name(), "GE/param1.1");
  EXPECT_EQ(graph_def.node(2).input_size(), 2);
  EXPECT_EQ(graph_def.node(2).name(), "GE/greater-than-or-equal-to");
  EXPECT_EQ(graph_def.node(2).op(), "HloGreaterThanOrEqualTo");
}

TEST_F(HloTfGraphBuilderTest, EmbeddedComputationsDiamond) {
  // Create computations with a diamond-shaped callgraph.
  auto negate_computation = CreateNegateComputation();
  auto map1_computation = CreateMapComputation(negate_computation.get());
  auto map2_computation = CreateMapComputation(negate_computation.get());

  auto builder = HloComputation::Builder(TestName());
  auto param = builder.AddInstruction(
      HloInstruction::CreateParameter(0, r0f32_, "param0"));
  auto map1 = builder.AddInstruction(
      HloInstruction::CreateMap(r0f32_, {param}, map1_computation.get()));
  auto map2 = builder.AddInstruction(
      HloInstruction::CreateMap(r0f32_, {param}, map2_computation.get()));
  builder.AddInstruction(
      HloInstruction::CreateBinary(r0f32_, HloOpcode::kAdd, map1, map2));
  auto computation = builder.Build();
  TF_CHECK_OK(generator_.AddComputation(*computation));
  EXPECT_GT(generator_.GetGraphDef().node_size(), 0);
}

}  // namespace
}  // namespace tools
}  // namespace xla
