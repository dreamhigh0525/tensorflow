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

#ifndef TENSORFLOW_CONTRIB_TENSORRT_SEGMENT_SEGMENT_H_
#define TENSORFLOW_CONTRIB_TENSORRT_SEGMENT_SEGMENT_H_

#include <set>
#include <vector>

#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

namespace tensorrt {
namespace segment {

using SegmentNodesVector = std::vector<std::pair<std::set<string>, string>>;
class Node;
class Graph;
class Edge {
 public:
  Edge(int id, Node* src, int src_port, Node* dst, int dst_port,
       bool is_control = false)
      : id_(id),
        src_(src),
        src_port_(src_port),
        dst_(dst),
        dst_port_(dst_port),
        control_(is_control){};
  Node* src() const { return src_; }
  Node* dst() const { return dst_; }
  int src_output() const { return src_port_; }
  int dst_input() const { return dst_port_; }
  int id() const { return id_; }
  bool IsControlEdge() const { return control_; }
  ~Edge() {}

 private:
  int id_;
  Node* src_;
  int src_port_;
  Node* dst_;
  int dst_port_;
  bool control_;
};
class Node {
  friend class Graph;

 public:
  Node(const tensorflow::Node* node, const int id);
  const std::vector<Edge*>& in_edges() const { return in_edges_; };
  const std::vector<Edge*>& out_edges() const { return out_edges_; };
  std::vector<Node*> in_nodes() const {
    std::vector<Node*> res;
    res.reserve(in_edges_.size());
    for (const auto e : in_edges_) {
      if (e) res.push_back(e->src());
    }
    return res;
  }
  const string& name() const { return node_->name(); }
  const tensorflow::Node* tf_node() const { return node_; }
  int id() const { return id_; }

 private:
  const tensorflow::Node* node_;
  std::vector<Edge*> in_edges_;
  std::vector<Edge*> out_edges_;
  int id_;
};

class Graph {
 public:
  Graph(const tensorflow::Graph* g);
  void AddControlEdge(Node* src, Node* dst);
  void AddEdge(Node* src, int out_port, Node* dst, int in_port);
  void RemoveEdge(const Edge*);
  Node* FindNodeId(int node_id) {
    if (node_id < 0 || node_id > (int)nodes_.size()) return nullptr;
    return nodes_[node_id];
  }
  ~Graph();
  int num_node_ids() const { return nodes_.size(); }
  const Node* source_node() const {
    return nodes_[tensorflow::Graph::kSourceId];
  }
  const Node* sink_node() const { return nodes_[tensorflow::Graph::kSinkId]; }

 private:
  const tensorflow::Graph* g_;
  std::vector<Node*> nodes_;
  std::vector<Edge*> edges_;
  std::set<int> edge_ids_;
  std::set<int> node_ids_;
};
struct SegmentOptions {
  // Segment must contain at least this many nodes.
  int minimum_segment_size = 2;
  std::set<string> exclude_node_list;
};

// // Get the subgraphs of a graph that can be handled by TensorRT.
// //
// // @param gdef The GraphDef describing the network
// // @param candidate_fn A function that returns true for a NodeDef if
// // that node can be handled by TensorRT.
// // @param segments Returns the TensorRT segments/subgraphs. Each entry
// // in the vector describes a subgraph by giving a set of the names of
// // all the NodeDefs in that subgraph.
// // @return the status.
tensorflow::Status SegmentGraph(
    const tensorflow::GraphDef& gdef,
    const std::function<bool(const tensorflow::Node*)>& candidate_fn,
    const SegmentOptions& options, SegmentNodesVector* segments);

// Get the subgraphs of a graph that can be handled by TensorRT.
//
// @param graph tensorflow::Graph of the network
// @param candidate_fn A function that returns true for a Node* if
// that node can be handled by TensorRT.
// @param segments Returns the TensorRT segments/subgraphs. Each entry
// in the vector describes a subgraph by giving a set of the names of
// all the NodeDefs in that subgraph.
// @return the status.
tensorflow::Status SegmentGraph(
    tensorflow::Graph* graph,
    const std::function<bool(const tensorflow::Node*)>& candidate_fn,
    const SegmentOptions& options, SegmentNodesVector* segments);

}  // namespace segment
}  // namespace tensorrt
}  // namespace tensorflow

#endif  // TENSORFLOW_CONTRIB_TENSORRT_SEGMENT_SEGMENT_H_
