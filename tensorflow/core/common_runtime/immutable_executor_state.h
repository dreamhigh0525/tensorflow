/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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
#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_IMMUTABLE_EXECUTOR_STATE_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_IMMUTABLE_EXECUTOR_STATE_H_

#include <deque>
#include <memory>
#include <vector>

#include "tensorflow/core/common_runtime/executor.h"
#include "tensorflow/core/common_runtime/graph_view.h"
#include "tensorflow/core/common_runtime/pending_counts.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/flatset.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

class Graph;

// Represents the state of an executor (graph and control flow information)
// that is immutable throughout execution.
//
// TODO(b/152651962): Add independent unit tests for this class.
class ImmutableExecutorState {
 public:
  struct FrameInfo {
    FrameInfo()
        : input_count(0),
          total_inputs(0),
          pending_counts(nullptr),
          nodes(nullptr) {}

    // The total number of inputs to a frame.
    int input_count;

    // The total number of input tensors of a frame.
    // == sum(nodes[*].num_inputs()) where nodes are the nodes in the frame.
    int total_inputs;

    // Used to determine the next place to allocate space in the
    // pending_counts data structure we'll eventually construct
    PendingCounts::Layout pending_counts_layout;

    // Each frame has its own PendingCounts only for the nodes in the frame.
    std::unique_ptr<PendingCounts> pending_counts;

    // The nodes in a frame. Used only for debugging.
    std::unique_ptr<std::vector<const NodeItem*>> nodes;
  };

  explicit ImmutableExecutorState(const LocalExecutorParams& p)
      : params_(p), gview_() {}
  ~ImmutableExecutorState();

  Status Initialize(const Graph& graph);

  // Process all Nodes in the current graph, attempting to infer the
  // memory allocation attributes to be used wherever they may allocate
  // a tensor buffer.
  Status SetAllocAttrs();

  const LocalExecutorParams& params() const { return params_; }
  const GraphView& graph_view() const { return gview_; }
  const std::vector<PendingCounts::Handle>& pending_ids() const {
    return pending_ids_;
  }
  const std::vector<const NodeItem*>& root_nodes() const { return root_nodes_; }

  const FrameInfo* get_frame_info(const string& frame_name) const {
    auto it_frame_info = frame_info_.find(frame_name);
    if (it_frame_info == frame_info_.end()) {
      return nullptr;
    } else {
      return it_frame_info->second;
    }
  }

  const FrameInfo& get_root_frame_info() const { return *root_frame_info_; }

  bool requires_control_flow_support() const { return requires_control_flow_; }

 private:
  struct ControlFlowInfo {
    gtl::FlatSet<string> unique_frame_names;
    std::vector<string> frame_names;
  };

  static Status BuildControlFlowInfo(const Graph* graph,
                                     ControlFlowInfo* cf_info);
  void InitializePending(const Graph* graph, const ControlFlowInfo& cf_info);

  FrameInfo* EnsureFrameInfo(const string& fname);

  // Owned.
  LocalExecutorParams params_;
  GraphView gview_;
  bool requires_control_flow_;
  std::vector<PendingCounts::Handle> pending_ids_;

  // Root nodes (with no in edges) that should form the initial ready queue
  std::vector<const NodeItem*> root_nodes_;

  // Mapping from frame name to static information about the frame.
  // TODO(yuanbyu): We could cache it along with the graph so to avoid
  // the overhead of constructing it for each executor instance.
  gtl::FlatMap<string, FrameInfo*> frame_info_;
  const FrameInfo* root_frame_info_;  // Not owned.

  // Shallow copies of the constant tensors used in the graph.
  std::vector<Tensor> const_tensors_;

  TF_DISALLOW_COPY_AND_ASSIGN(ImmutableExecutorState);
};

}  // namespace tensorflow
#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_IMMUTABLE_EXECUTOR_STATE_H_
