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

#ifndef TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_DEPENDENCY_OPTIMIZER_H_
#define TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_DEPENDENCY_OPTIMIZER_H_

#include <unordered_set>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "tensorflow/core/grappler/mutable_graph_view.h"
#include "tensorflow/core/grappler/optimizers/graph_optimizer.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/protobuf/rewriter_config.pb.h"

namespace tensorflow {
namespace grappler {

// Optimize TF computations by removing control dependencies or re-arranging
// them to shorten the critical path for a model step or enable other
// optimizations, such as removing nodes that are effectively noops.
class DependencyOptimizer : public GraphOptimizer {
 public:
  DependencyOptimizer() {}
  explicit DependencyOptimizer(RewriterConfig::Toggle opt_level) {}
  ~DependencyOptimizer() override {}

  string name() const override { return "dependency_optimizer"; };

  Status Optimize(Cluster* cluster, const GrapplerItem& item,
                  GraphDef* optimized_graph) override;

  void Feedback(Cluster* cluster, const GrapplerItem& item,
                const GraphDef& optimized_graph, double result) override;

 private:
  // Returns true if bypassing node does not increase the number of edges or
  // number of edges crossing a device boundary.
  bool BypassingNodeIsBeneficial(
      const NodeDef& node, int num_controlling_fanins,
      const absl::flat_hash_set<MutableGraphView::Edge>& fanin_edges,
      const absl::flat_hash_set<MutableGraphView::Edge>& fanout_edges) const;
  int NumEdgesIfBypassed(
      const NodeDef& node, int num_controlling_fanins,
      const absl::flat_hash_set<MutableGraphView::Edge>& fanin_edges,
      const absl::flat_hash_set<MutableGraphView::Edge>& fanout_edges,
      int num_unique_fanout_nodes) const;
  // Returns true if node is not an Identity node or if it is an Identity
  // that is safe to remove.
  bool SafeToRemoveIdentity(const NodeDef& node) const;
  // Returns true if it is safe to convert node to NoOp.
  bool SafeToConvertToNoOp(const NodeDef& node) const;
  // Tries to optimize the node with the given node name, possibly additional
  // optimizations by inserting nodes in nodes_to_simplify, and pruning nodes by
  // inserting them in nodes_to_delete.
  Status OptimizeNode(const string& node_name,
                      SetVector<string>* nodes_to_simplify,
                      absl::flat_hash_set<string>* nodes_to_delete);
  // Eliminates redundant control dependencies by computing the transitive
  // reduction of the graph.
  Status TransitiveReduction();
  // Main driver of dependency optimizations.
  Status OptimizeDependencies();
  // Replaces multiple cross-device control edges from the same device with a
  // single control edge.
  Status GroupCrossDeviceControlEdges();

  bool fetch_nodes_known_;
  std::unordered_set<string> nodes_to_preserve_;
  std::unique_ptr<MutableGraphView> graph_view_;
};

}  // end namespace grappler
}  // end namespace tensorflow

#endif  // TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_DEPENDENCY_OPTIMIZER_H_
