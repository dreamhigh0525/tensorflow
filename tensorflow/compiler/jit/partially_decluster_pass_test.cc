/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/jit/partially_decluster_pass.h"

#include "tensorflow/cc/framework/ops.h"
#include "tensorflow/cc/ops/array_ops.h"
#include "tensorflow/cc/ops/control_flow_ops_internal.h"
#include "tensorflow/cc/ops/function_ops.h"
#include "tensorflow/cc/ops/sendrecv_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/compiler/jit/defs.h"
#include "tensorflow/compiler/jit/xla_cluster_util.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/graph/graph_def_builder.h"
#include "tensorflow/core/graph/graph_def_builder_util.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {
REGISTER_OP("FakeNullary").Output("out: float");

REGISTER_OP("FakeBinary")
    .Input("host_in: float")
    .Input("device_in: float")
    .Output("host_out: float")
    .Output("device_out: float");

REGISTER_OP("FakeResourceVar").Output("out: resource");

REGISTER_OP("FakeResourceUpdate")
    .Input("in: resource")
    .Output("out: resource")
    .Output("something_else: float");

class FakeBinaryOp : public OpKernel {
 public:
  explicit FakeBinaryOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* ctx) override { CHECK(false); }
};

class FakeResourceVarUpdateOp : public OpKernel {
 public:
  explicit FakeResourceVarUpdateOp(OpKernelConstruction* context)
      : OpKernel(context) {}

  void Compute(OpKernelContext* ctx) override { CHECK(false); }
};

REGISTER_KERNEL_BUILDER(Name("FakeBinary")
                            .Device(DEVICE_CPU)
                            .HostMemory("host_in")
                            .HostMemory("host_out"),
                        FakeBinaryOp);

REGISTER_KERNEL_BUILDER(Name("FakeResourceVarUpdate")
                            .Device(DEVICE_CPU)
                            .HostMemory("something_else"),
                        FakeResourceVarUpdateOp);

Status PartiallyDecluster(std::unique_ptr<Graph>* graph) {
  FixupSourceAndSinkEdges(graph->get());
  // Assign all nodes to the CPU device.
  static const char* kCpuDevice = "/job:localhost/replica:0/task:0/cpu:0";
  for (Node* n : (*graph)->nodes()) {
    n->set_assigned_device_name(kCpuDevice);
  }

  GraphOptimizationPassOptions opt_options;
  opt_options.graph = graph;
  PartiallyDeclusterPass pass;
  return pass.Run(opt_options);
}

const Node* FindNodeByName(const Graph& graph, const string& name) {
  for (const Node* node : graph.nodes()) {
    if (node->name() == name) {
      return node;
    }
  }
  return nullptr;
}

bool GetInputsForNode(const Graph& graph, const string& node_name,
                      std::vector<Node*>* inputs) {
  const Node* node = FindNodeByName(graph, node_name);
  if (node == nullptr) {
    return false;
  }
  for (const Edge* e : node->in_edges()) {
    inputs->push_back(e->src());
  }
  std::sort(inputs->begin(), inputs->end(), NodeComparatorName());
  return true;
}

TEST(PartiallyDeclusterPassTest, ClusteredAndUnclustered) {
  std::unique_ptr<Graph> graph(new Graph(OpRegistry::Global()));
  {
    GraphDefBuilder builder(GraphDefBuilder::kFailImmediately);
    Node* input =
        ops::SourceOp("FakeNullary", builder.opts().WithName("Input"));
    Node* clustered_producer =
        ops::BinaryOp("FakeBinary", input, input,
                      builder.opts().WithName("ClusteredProducer"));
    ops::BinaryOp("FakeBinary", clustered_producer, input,
                  builder.opts().WithName("UnclusteredConsumer"));
    Node* clustered_consumer =
        ops::BinaryOp("FakeBinary", {clustered_producer, 1}, input,
                      builder.opts().WithName("ClusteredConsumer"));
    clustered_producer->AddAttr(kXlaClusterAttr, "cluster_0");
    clustered_consumer->AddAttr(kXlaClusterAttr, "cluster_0");
    TF_EXPECT_OK(GraphDefBuilderToGraph(builder, graph.get()));
  }

  TF_ASSERT_OK(PartiallyDecluster(&graph));
  std::vector<Node*> unclustered_consumer_inputs;
  ASSERT_TRUE(GetInputsForNode(*graph, "UnclusteredConsumer",
                               &unclustered_consumer_inputs));
  ASSERT_EQ(unclustered_consumer_inputs.size(), 2);
  EXPECT_EQ(unclustered_consumer_inputs[0]->name(),
            "ClusteredProducer/declustered");
  EXPECT_EQ(unclustered_consumer_inputs[1]->name(), "Input");

  std::vector<Node*> clustered_consumer_inputs;
  ASSERT_TRUE(GetInputsForNode(*graph, "ClusteredConsumer",
                               &clustered_consumer_inputs));
  ASSERT_EQ(clustered_consumer_inputs.size(), 2);
  EXPECT_EQ(clustered_consumer_inputs[0]->name(), "ClusteredProducer");
  EXPECT_EQ(clustered_consumer_inputs[1]->name(), "Input");
}

TEST(PartiallyDeclusterPassTest, DifferentClusters) {
  std::unique_ptr<Graph> graph(new Graph(OpRegistry::Global()));
  {
    GraphDefBuilder builder(GraphDefBuilder::kFailImmediately);
    Node* input =
        ops::SourceOp("FakeNullary", builder.opts().WithName("Input"));
    Node* clustered_producer =
        ops::BinaryOp("FakeBinary", input, input,
                      builder.opts().WithName("ClusteredProducer"));
    Node* consumer_in_different_cluster =
        ops::BinaryOp("FakeBinary", clustered_producer, input,
                      builder.opts().WithName("ConsumerInDifferentCluster"));
    Node* clustered_consumer =
        ops::BinaryOp("FakeBinary", input, {clustered_producer, 1},
                      builder.opts().WithName("ClusteredConsumer"));
    clustered_producer->AddAttr(kXlaClusterAttr, "cluster_0");
    clustered_consumer->AddAttr(kXlaClusterAttr, "cluster_0");
    consumer_in_different_cluster->AddAttr(kXlaClusterAttr, "cluster_1");
    TF_EXPECT_OK(GraphDefBuilderToGraph(builder, graph.get()));
  }

  TF_ASSERT_OK(PartiallyDecluster(&graph));
  std::vector<Node*> inputs;
  ASSERT_TRUE(GetInputsForNode(*graph, "ConsumerInDifferentCluster", &inputs));
  ASSERT_EQ(inputs.size(), 2);
  EXPECT_EQ(inputs[0]->name(), "ClusteredProducer/declustered");
  EXPECT_EQ(inputs[1]->name(), "Input");
}

TEST(PartiallyDeclusterPassTest, DontDeclusterIfUserIsDeviceMem) {
  std::unique_ptr<Graph> graph(new Graph(OpRegistry::Global()));
  {
    GraphDefBuilder builder(GraphDefBuilder::kFailImmediately);
    Node* input =
        ops::SourceOp("FakeNullary", builder.opts().WithName("Input"));
    Node* clustered_producer =
        ops::BinaryOp("FakeBinary", input, input,
                      builder.opts().WithName("ClusteredProducer"));
    // The first input is hostmem and the second input is devicemem.
    Node* consumer_in_different_cluster =
        ops::BinaryOp("FakeBinary", input, clustered_producer,
                      builder.opts().WithName("ConsumerInDifferentCluster"));
    Node* clustered_consumer =
        ops::BinaryOp("FakeBinary", input, {clustered_producer, 1},
                      builder.opts().WithName("ClusteredConsumer"));
    clustered_producer->AddAttr(kXlaClusterAttr, "cluster_0");
    clustered_consumer->AddAttr(kXlaClusterAttr, "cluster_0");
    consumer_in_different_cluster->AddAttr(kXlaClusterAttr, "cluster_1");
    TF_EXPECT_OK(GraphDefBuilderToGraph(builder, graph.get()));
  }

  TF_ASSERT_OK(PartiallyDecluster(&graph));
  std::vector<Node*> inputs;
  ASSERT_TRUE(GetInputsForNode(*graph, "ConsumerInDifferentCluster", &inputs));
  ASSERT_EQ(inputs.size(), 2);
  EXPECT_EQ(inputs[0]->name(), "ClusteredProducer");
  EXPECT_EQ(inputs[1]->name(), "Input");
}

TEST(PartiallyDeclusterPassTest, DontDuplicateResourceVarOps) {
  std::unique_ptr<Graph> graph(new Graph(OpRegistry::Global()));
  {
    GraphDefBuilder builder(GraphDefBuilder::kFailImmediately);
    Node* input =
        ops::SourceOp("FakeNullary", builder.opts().WithName("Input"));
    Node* resource_var = ops::SourceOp("FakeResourceVar",
                                       builder.opts().WithName("ResourceVar"));
    Node* clustered_producer =
        ops::UnaryOp("FakeResourceUpdate", resource_var,
                     builder.opts().WithName("ClusteredProducer"));
    Node* consumer_in_different_cluster =
        ops::BinaryOp("FakeBinary", {clustered_producer, 1}, input,
                      builder.opts().WithName("ConsumerInDifferentCluster"));
    Node* clustered_consumer =
        ops::BinaryOp("FakeBinary", input, {clustered_producer, 1},
                      builder.opts().WithName("ClusteredConsumer"));
    clustered_producer->AddAttr(kXlaClusterAttr, "cluster_0");
    clustered_consumer->AddAttr(kXlaClusterAttr, "cluster_0");
    consumer_in_different_cluster->AddAttr(kXlaClusterAttr, "cluster_1");
    TF_EXPECT_OK(GraphDefBuilderToGraph(builder, graph.get()));
  }

  TF_ASSERT_OK(PartiallyDecluster(&graph));
  std::vector<Node*> inputs;
  ASSERT_TRUE(GetInputsForNode(*graph, "ConsumerInDifferentCluster", &inputs));
  ASSERT_EQ(inputs.size(), 2);
  EXPECT_EQ(inputs[0]->name(), "ClusteredProducer");
  EXPECT_EQ(inputs[1]->name(), "Input");
}

TEST(PartiallyDeclusterPassTest, DeclusterDependentNodes) {
  std::unique_ptr<Graph> graph(new Graph(OpRegistry::Global()));
  {
    GraphDefBuilder builder(GraphDefBuilder::kFailImmediately);
    Node* input =
        ops::SourceOp("FakeNullary", builder.opts().WithName("Input"));
    Node* clustered_producer_0 =
        ops::BinaryOp("FakeBinary", input, input,
                      builder.opts().WithName("ClusteredProducer0"));
    Node* clustered_producer_1 =
        ops::BinaryOp("FakeBinary", clustered_producer_0, input,
                      builder.opts().WithName("ClusteredProducer1"));
    ops::BinaryOp("FakeBinary", clustered_producer_1, input,
                  builder.opts().WithName("UnclusteredConsumer"));
    Node* clustered_consumer =
        ops::BinaryOp("FakeBinary", {clustered_producer_1, 1}, input,
                      builder.opts().WithName("ClusteredConsumer"));
    clustered_producer_0->AddAttr(kXlaClusterAttr, "cluster_0");
    clustered_producer_1->AddAttr(kXlaClusterAttr, "cluster_0");
    clustered_consumer->AddAttr(kXlaClusterAttr, "cluster_0");
    TF_EXPECT_OK(GraphDefBuilderToGraph(builder, graph.get()));
  }

  TF_ASSERT_OK(PartiallyDecluster(&graph));
  std::vector<Node*> unclustered_consumer_inputs, declustered_producer_1_inputs;

  ASSERT_TRUE(GetInputsForNode(*graph, "UnclusteredConsumer",
                               &unclustered_consumer_inputs));
  ASSERT_EQ(unclustered_consumer_inputs.size(), 2);
  EXPECT_EQ(unclustered_consumer_inputs[0]->name(),
            "ClusteredProducer1/declustered");
  EXPECT_EQ(unclustered_consumer_inputs[1]->name(), "Input");

  ASSERT_TRUE(GetInputsForNode(*graph, "ClusteredProducer1/declustered",
                               &declustered_producer_1_inputs));
  ASSERT_EQ(declustered_producer_1_inputs.size(), 2);
  EXPECT_EQ(declustered_producer_1_inputs[0]->name(),
            "ClusteredProducer0/declustered");
  EXPECT_EQ(declustered_producer_1_inputs[1]->name(), "Input");
}
}  // namespace
}  // namespace tensorflow
