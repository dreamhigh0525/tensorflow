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

#ifndef TENSORFLOW_GRAPPLER_OPTIMIZERS_ARITHMETIC_OPTIMIZER_H_
#define TENSORFLOW_GRAPPLER_OPTIMIZERS_ARITHMETIC_OPTIMIZER_H_

#include <unordered_set>
#include "tensorflow/core/grappler/optimizers/graph_optimizer.h"
#include "tensorflow/core/grappler/utils.h"

namespace tensorflow {
namespace grappler {

// Optimize TF computations by reducing the arithmetic complexity required to
// run a model.
class ArithmeticOptimizer : public GraphOptimizer {
 public:
  ArithmeticOptimizer() {}
  ~ArithmeticOptimizer() override {}

  string name() const override { return "arithmetic_optimizer"; };

  Status Optimize(Cluster* cluster, const GrapplerItem& item,
                  GraphDef* optimized_graph) override;

  void Feedback(Cluster* cluster, const GrapplerItem& item,
                const GraphDef& optimized_graph, double result) override;

 private:
  bool CanDedup(const NodeDef& node) const;
  void DedupComputations(GraphDef* optimized_graph) const;
  // Runs peep-hole optimizations on `optimized_graph`, e.g., removing inverse
  // transposes.
  void SimplifyArithmeticOps(GraphDef* optimized_graph) const;
  // Tries to simplify the expression that roots at `node` and replaces the uses
  // of `node` to the simplified expression. Returns the simplified node or
  // nullptr if no simplification is performed.
  //
  // `node_map` stores the mapping from node names to NodeDef*, and will be
  // updated according to the rewrite.
  //
  // `new_nodes` will be populated with the new nodes this function creates and
  // updates. The caller can push these nodes into the simplification queue to
  // optimize them further.
  const NodeDef* TrySimplifyAndReplaceUses(
      const NodeDef* node, GraphDef* graph_def, NodeMap* node_map,
      std::vector<const NodeDef*>* new_nodes) const;

  std::unordered_set<string> nodes_to_preserve_;
};

}  // end namespace grappler
}  // end namespace tensorflow

#endif  // TENSORFLOW_GRAPPLER_OPTIMIZERS_ARITHMETIC_OPTIMIZER_H_
