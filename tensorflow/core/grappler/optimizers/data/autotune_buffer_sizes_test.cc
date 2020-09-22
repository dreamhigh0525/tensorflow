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

#include "tensorflow/core/grappler/optimizers/data/autotune_buffer_sizes.h"

#include "tensorflow/core/framework/attr_value_util.h"
#include "tensorflow/core/framework/function_testlib.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/optimizers/data/graph_test_utils.h"
#include "tensorflow/core/grappler/optimizers/data/graph_utils.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace grappler {
namespace {

class SimpleInject : public ::testing::TestWithParam<string> {};

TEST_P(SimpleInject, AutotuneBufferSizesTest) {
  const string async_dataset = GetParam();
  using test::function::NDef;
  GrapplerItem item;
  if (async_dataset == "map") {
    item.graph = test::function::GDef(
        {NDef("start", "Const", {}, {{"value", 0}, {"dtype", DT_INT32}}),
         NDef("stop", "Const", {}, {{"value", 10}, {"dtype", DT_INT32}}),
         NDef("step", "Const", {}, {{"value", 1}, {"dtype", DT_INT32}}),
         NDef("range", "RangeDataset", {"start", "stop", "step"}, {}),
         NDef("num_parallel_calls", "Const", {},
              {{"value", 1}, {"dtype", DT_INT32}}),
         graph_tests_utils::MakeParallelMapNode(
             "map", "range", "num_parallel_calls", "XTimesTwo",
             /*sloppy=*/false)},
        // FunctionLib
        {
            test::function::XTimesTwo(),
        });
  } else if (async_dataset == "interleave") {
    item.graph = test::function::GDef(
        {NDef("start", "Const", {}, {{"value", 0}, {"dtype", DT_INT32}}),
         NDef("stop", "Const", {}, {{"value", 10}, {"dtype", DT_INT32}}),
         NDef("step", "Const", {}, {{"value", 1}, {"dtype", DT_INT32}}),
         NDef("range", "RangeDataset", {"start", "stop", "step"}, {}),
         NDef("cycle_length", "Const", {}, {{"value", 1}, {"dtype", DT_INT32}}),
         NDef("block_length", "Const", {}, {{"value", 1}, {"dtype", DT_INT32}}),
         NDef("num_parallel_calls", "Const", {},
              {{"value", 1}, {"dtype", DT_INT32}}),
         graph_tests_utils::MakeParallelInterleaveV2Node(
             "interleave", "range", "cycle_length", "block_length",
             "num_parallel_calls", "XTimesTwo", /*sloppy=*/false)},
        // FunctionLib
        {
            test::function::XTimesTwo(),
        });
  } else if (async_dataset == "map_and_batch") {
    item.graph = test::function::GDef(
        {NDef("start", "Const", {}, {{"value", 0}, {"dtype", DT_INT32}}),
         NDef("stop", "Const", {}, {{"value", 10}, {"dtype", DT_INT32}}),
         NDef("step", "Const", {}, {{"value", 1}, {"dtype", DT_INT32}}),
         NDef("range", "RangeDataset", {"start", "stop", "step"}, {}),
         NDef("batch_size", "Const", {}, {{"value", 32}, {"dtype", DT_INT64}}),
         NDef("num_parallel_calls", "Const", {},
              {{"value", 1}, {"dtype", DT_INT64}}),
         NDef("drop_remainder", "Const", {},
              {{"value", false}, {"dtype", DT_BOOL}}),
         graph_tests_utils::MakeMapAndBatchNode(
             "map_and_batch", "range", "batch_size", "num_parallel_calls",
             "drop_remainder", "XTimesTwo")},
        // FunctionLib
        {
            test::function::XTimesTwo(),
        });
  }

  AutotuneBufferSizes optimizer;
  GraphDef output;
  TF_ASSERT_OK(optimizer.Optimize(nullptr, item, &output));

  EXPECT_TRUE(graph_utils::ContainsNodeWithOp("PrefetchDataset", output));
  int index = graph_utils::FindGraphNodeWithOp("PrefetchDataset", output);
  const NodeDef prefetch_node = output.node(index);
  EXPECT_FALSE(prefetch_node.attr().at("legacy_autotune").b());
  EXPECT_EQ(prefetch_node.input_size(), 2);
  NodeDef async_node = output.node(
      graph_utils::FindGraphNodeWithName(prefetch_node.input(0), output));
  EXPECT_EQ(async_node.name(), async_dataset);
  NodeDef buffer_size_val = output.node(
      graph_utils::FindGraphNodeWithName(prefetch_node.input(1), output));
  EXPECT_EQ(buffer_size_val.attr().at("value").tensor().int64_val(0), -1);
}

INSTANTIATE_TEST_SUITE_P(Test, SimpleInject,
                         ::testing::Values("map", "interleave",
                                           "map_and_batch"));

class MultipleNodes : public ::testing::TestWithParam<std::tuple<bool, int64>> {
};

TEST_P(MultipleNodes, AutotuneBufferSizesTest) {
  const bool legacy_autotune = std::get<0>(GetParam());
  const int64 initial_buffer_size = std::get<1>(GetParam());

  GrapplerItem item;
  MutableGraphView graph(&item.graph);

  NodeDef *start_val = graph_utils::AddScalarConstNode<int64>(0, &graph);
  NodeDef *stop_val = graph_utils::AddScalarConstNode<int64>(10, &graph);
  NodeDef *step_val = graph_utils::AddScalarConstNode<int64>(1, &graph);

  std::vector<string> range_inputs(3);
  range_inputs[0] = start_val->name();
  range_inputs[1] = stop_val->name();
  range_inputs[2] = step_val->name();
  std::vector<std::pair<string, AttrValue>> range_attrs;
  NodeDef *range_node = graph_utils::AddNode("range", "RangeDataset",
                                             range_inputs, range_attrs, &graph);

  NodeDef *parallelism_val = graph_utils::AddScalarConstNode<int64>(1, &graph);
  std::vector<string> map_inputs1(2);
  map_inputs1[0] = range_node->name();
  map_inputs1[1] = parallelism_val->name();
  std::vector<std::pair<string, AttrValue>> map_attrs(4);
  AttrValue attr_val;
  SetAttrValue("value", &attr_val);
  map_attrs[0] = std::make_pair("f", attr_val);
  map_attrs[1] = std::make_pair("Targuments", attr_val);
  map_attrs[2] = std::make_pair("output_types", attr_val);
  map_attrs[3] = std::make_pair("output_shapes", attr_val);
  NodeDef *map_node1 = graph_utils::AddNode("map1", "ParallelMapDatasetV2",
                                            map_inputs1, map_attrs, &graph);

  NodeDef *buffer_size_val =
      graph_utils::AddScalarConstNode<int64>(initial_buffer_size, &graph);
  std::vector<string> prefetch_inputs(2);
  prefetch_inputs[0] = map_node1->name();
  prefetch_inputs[1] = buffer_size_val->name();
  std::vector<std::pair<string, AttrValue>> prefetch_attrs(2);
  AttrValue legacy_autotune_attr;
  SetAttrValue(legacy_autotune, &legacy_autotune_attr);
  prefetch_attrs[0] = std::make_pair("legacy_autotune", legacy_autotune_attr);
  AttrValue buffer_size_min_attr;
  SetAttrValue(0, &buffer_size_min_attr);
  prefetch_attrs[1] = std::make_pair("buffer_size_min", buffer_size_min_attr);
  NodeDef *prefetch_node = graph_utils::AddNode(
      "prefetch", "PrefetchDataset", prefetch_inputs, prefetch_attrs, &graph);

  std::vector<string> map_inputs2(2);
  map_inputs2[0] = prefetch_node->name();
  map_inputs2[1] = parallelism_val->name();
  graph_utils::AddNode("map2", "ParallelMapDatasetV2", map_inputs2, map_attrs,
                       &graph);

  EXPECT_EQ(item.graph.node_size(), 9);

  AutotuneBufferSizes optimizer;
  GraphDef output;
  TF_ASSERT_OK(optimizer.Optimize(nullptr, item, &output));
  EXPECT_EQ(output.node_size(), 11);

  std::vector<int> prefetch_indices =
      graph_utils::FindAllGraphNodesWithOp("PrefetchDataset", output);
  EXPECT_EQ(prefetch_indices.size(), 2);
  NodeDef new_prefetch_node1 = output.node(prefetch_indices[0]);
  NodeDef new_prefetch_node2 = output.node(prefetch_indices[1]);

  EXPECT_EQ(new_prefetch_node1.input_size(), 2);
  EXPECT_FALSE(new_prefetch_node1.attr().at("legacy_autotune").b());
  EXPECT_EQ(new_prefetch_node1.attr().at("buffer_size_min").i(),
            (initial_buffer_size == -1 ? 0 : initial_buffer_size));
  NodeDef new_map_node1 = output.node(
      graph_utils::FindGraphNodeWithName(new_prefetch_node1.input(0), output));
  EXPECT_EQ(new_map_node1.name(), "map1");
  NodeDef new_buffer_size_val1 = output.node(
      graph_utils::FindGraphNodeWithName(new_prefetch_node1.input(1), output));
  EXPECT_EQ(new_buffer_size_val1.attr().at("value").tensor().int64_val(0), -1);

  EXPECT_EQ(new_prefetch_node2.input_size(), 2);
  EXPECT_FALSE(new_prefetch_node2.attr().at("legacy_autotune").b());
  NodeDef new_map_node2 = output.node(
      graph_utils::FindGraphNodeWithName(new_prefetch_node2.input(0), output));
  EXPECT_EQ(new_map_node2.name(), "map2");
  NodeDef new_buffer_size_val2 = output.node(
      graph_utils::FindGraphNodeWithName(new_prefetch_node2.input(1), output));
  EXPECT_EQ(new_buffer_size_val2.attr().at("value").tensor().int64_val(0), -1);
}

INSTANTIATE_TEST_SUITE_P(Test, MultipleNodes,
                         ::testing::Combine(::testing::Values(true, false),
                                            ::testing::Values(-1, 3)));

}  // namespace
}  // namespace grappler
}  // namespace tensorflow
