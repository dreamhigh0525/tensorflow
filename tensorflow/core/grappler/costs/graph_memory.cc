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

#include "tensorflow/core/grappler/costs/graph_memory.h"

#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/grappler/costs/graph_properties.h"

namespace tensorflow {
namespace grappler {

Status GraphMemory::InferStatically() {
  GraphProperties properties(item_);
  TF_RETURN_IF_ERROR(properties.InferStatically());
  return InferFromGraphProperties(&properties);
}

Status GraphMemory::InferDynamically(Cluster* cluster) {
  GraphProperties properties(item_);
  TF_RETURN_IF_ERROR(properties.InferDynamically(cluster));
  return InferFromGraphProperties(&properties);
}

Status GraphMemory::InferFromGraphProperties(GraphProperties* properties) {
  // Compute the worst case usage between initialization and normal mode.
  // TODO(bsteiner): we should consider persistent memory usage separately.
  int64 worst_case_init_mem_usage;
  int64 best_case_init_mem_usage;
  InferMemUsageForNodes(item_.InitOpsFanin(), properties,
                        &worst_case_init_mem_usage, &best_case_init_mem_usage);
  int64 worst_case_main_mem_usage;
  int64 best_case_main_mem_usage;
  InferMemUsageForNodes(item_.MainOpsFanin(), properties,
                        &worst_case_main_mem_usage, &best_case_main_mem_usage);

  worst_case_memory_usage_ =
      std::max(worst_case_init_mem_usage, worst_case_main_mem_usage);
  best_case_memory_usage_ =
      std::max(best_case_init_mem_usage, best_case_main_mem_usage);

  return Status::OK();
}

void GraphMemory::InferMemUsageForNodes(
    const std::vector<const NodeDef*>& nodes, GraphProperties* properties,
    int64* worst_case_memory_usage, int64* best_case_memory_usage) const {
  // TODO(bsteiner) refine this: we should consider the multidevice case.
  *worst_case_memory_usage = 0;
  *best_case_memory_usage = 0;
  for (const auto& node : item_.graph.node()) {
    // Estimate the memory required to store the tensors generated by the node.
    std::vector<OpInfo::TensorProperties> outputs =
        properties->GetOutputProperties(node.name());
    int64 node_memory_usage = InferMemUsageForNeighbors(outputs);

    // Worst case memory usage corresponds to the case where all the nodes are
    // alive.
    *worst_case_memory_usage += node_memory_usage;

    // Estimate the memory required to store the input tensors needed by the
    // node.
    std::vector<OpInfo::TensorProperties> inputs =
        properties->GetInputProperties(node.name());
    node_memory_usage += InferMemUsageForNeighbors(inputs);

    *best_case_memory_usage =
        std::max(*best_case_memory_usage, node_memory_usage);
  }
}

int64 GraphMemory::InferMemUsageForNeighbors(
    const std::vector<OpInfo::TensorProperties>& props) const {
  int64 neighbors_memory_usage = 0;
  for (const auto& prop : props) {
    DataType dtype = prop.dtype();
    int size = DataTypeSize(dtype);
    TensorShapeProto shape = prop.shape();
    if (shape.unknown_rank()) {
      // Can't infer the size if the rank is unknown, just skip.
      continue;
    }
    // If one of the dimensions is unknown statically, assume it's one.
    for (int i = 0; i < shape.dim_size(); ++i) {
      if (shape.dim(i).size() < 0) {
        shape.mutable_dim(i)->set_size(1);
      }
    }
    int num_elems = TensorShape(shape).num_elements();
    neighbors_memory_usage += num_elems * size;
  }
  return neighbors_memory_usage;
}

}  // end namespace grappler
}  // end namespace tensorflow
