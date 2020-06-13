/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/grappler/optimizers/data/hoist_discard.h"

#include "absl/container/flat_hash_set.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/mutable_graph_view.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/grappler/optimizers/custom_graph_optimizer_registry.h"
#include "tensorflow/core/grappler/optimizers/data/function_utils.h"
#include "tensorflow/core/grappler/optimizers/data/graph_utils.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/platform/protobuf.h"

namespace tensorflow {
namespace grappler {
namespace {

const std::unordered_set<string> kDataDiscarding = {
    "ShardDataset", "SkipDataset", "TakeDataset",
};

const std::unordered_set<string> kCardinalityPreserving = {
    "CacheDataset", "CacheDatasetV2", "PrefetchDataset",
    "MapDataset", "ParallelMapDataset", "ParallelMapDatasetV2",
};

bool IsDataDiscarding(const NodeDef& node) {
  auto iter = kDataDiscarding.find(node.op());
  if (iter == kDataDiscarding.end()) {
    return false;
  }
  return true;
}

bool IsCardinalityPreserving(const NodeDef& node) {
  auto iter = kCardinalityPreserving.find(node.op());
  if (iter == kCardinalityPreserving.end()) {
    return false;
  }
  auto attr_iter = node.attr().find("preserve_cardinality");
  if (attr_iter != node.attr().end() && !attr_iter->second.b()) {
    return false;
  }
  return true;
}

}  // namepsace

Status HoistDiscard::OptimizeAndCollectStats(Cluster* cluster,
                                             const GrapplerItem& item,
                                             GraphDef* output,
                                             OptimizationStats* stats) {
  *output = item.graph;
  MutableGraphView graph(output);
  bool updated;
  do {
    updated = false;
    for (int i = 0; i < graph.graph()->node_size(); i++) {
      auto node = graph.graph()->mutable_node(i);
      if (IsDataDiscarding(*node)) {
        NodeDef* start = node;
        NodeDef* start_parent = graph_utils::GetInputNode(*start, graph);
        while (IsCardinalityPreserving(*start_parent)) {
          start = start_parent;
          start_parent = graph_utils::GetInputNode(*start, graph);
        }
        if (start->name() == node->name()) {
          continue;
        }
        auto parent = graph_utils::GetInputNode(*node, graph);
        TF_RETURN_IF_ERROR(graph.UpdateFanouts(node->name(), parent->name()));
        if (!absl::StartsWith(node->name(), "hoist_discard/")) {
          TF_RETURN_IF_ERROR(graph.UpdateNodeName(node->name(),
            strings::StrCat("hoist_discard/", node->name()), false));
        }
        for (const auto& attr_name : {"output_types", "output_shapes"}) {
          graph_utils::CopyAttribute(attr_name, *start_parent,
                                     node);
        }
        *node->mutable_input(0) = start_parent->name();
        *start->mutable_input(0) = node->name();
        updated = true;
        break;
      }
    }
  } while (updated);
  return Status::OK();
}

void HoistDiscard::Feedback(Cluster* cluster, const GrapplerItem& item,
                            const GraphDef& optimize_output,
                            double result) {
  // no-op
}

REGISTER_GRAPH_OPTIMIZER_AS(HoistDiscard, "hoist_discard");

}  // namespace grappler
}  // namespace tensorflow
