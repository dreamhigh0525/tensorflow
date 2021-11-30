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

#ifndef TENSORFLOW_CORE_GRAPH_COSTMODEL_H_
#define TENSORFLOW_CORE_GRAPH_COSTMODEL_H_

#include <unordered_map>
#include <vector>

#include "tensorflow/core/framework/cost_graph.pb.h"
#include "tensorflow/core/framework/step_stats.pb.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/graph/types.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/protobuf.h"

namespace tensorflow {
typedef std::unordered_map<StringPiece, int32, StringPieceHasher>
    NodeNameToCostIdMap;

class StepStats;

// CostModel keeps track of the following runtime statistics for nodes
// of a single Graph:
//    * The total number of times a node has executed.
//    * The accumulated execution time (in microseconds) of a node.
//    * The accumulated size (in bytes) of each node's output.
//
// This class is NOT thread-safe.
class CostModel {
 public:
  // If "global" is true, maintains costs based on Node::cost_id, otherwise
  // maintains costs based on Node::id.
  explicit CostModel(bool is_global) : is_global_(is_global) {
    unknown_shape_.set_unknown_rank(true);
  }

  // Assigns min_count_ as a function of the median count for a Node.
  // This value is then used for suppressing the time/size costs of
  // infrequent operations.
  // NOTE(tucker): Maybe this should move to a subclass of CostModel.
  void SuppressInfrequent();

  bool is_global() const { return is_global_; }

  inline int Id(const Node* n) const {
    if (is_global_) {
      return n->cost_id();
    } else {
      return n->id();
    }
  }

  inline int GlobalId(const Node* n, int offset) const {
    if (is_global_) {
      return n->cost_id();
    } else {
      return n->id() + offset;
    }
  }

  // Initializes cost model for 'g'.
  void InitFromGraph(const Graph& g);

  // Merges costs from cm.
  // REQUIRES: is_global_ is true for this and for "cm"
  void MergeFromGlobal(const CostModel& cm);

  // Merges costs from "cm", which has been computed relative to "g".
  // REQUIRES: is_global_ is true for this, and false for "cm".
  void MergeFromLocal(const Graph& g, const CostModel& cm);

  void MergeFromStats(const NodeNameToCostIdMap& map, const StepStats& ss);

  // Sets the number of outputs of "node".
  void SetNumOutputs(const Node* node, int num_outputs);

  // Records that "node" has executed "num_count" more times.
  void RecordCount(const Node* node, int num_count);

  // Returns how many times "node" has been executed.
  int32 TotalCount(const Node* node) const;

  // Records that "output_slot" of "node" has produced tensors of
  // aggregated "bytes".
  void RecordSize(const Node* node, int output_slot, Bytes bytes);

  // Returns total bytes of tensors produced by "node"s output slot.
  Bytes TotalBytes(const Node* node, int output_slot) const;

  // Returns a prediction for the size of the tensor at the
  // output_slot produced by one execution of "node".
  Bytes SizeEstimate(const Node* node, int output_slot) const;

  // Records that Executions of "node" have taken "time" microseconds.
  void RecordTime(const Node* node, Microseconds time);

  // Returns the total execution time for "node".
  Microseconds TotalTime(const Node* node) const;

  // Returns a prediction for one execution of "node".
  Microseconds TimeEstimate(const Node* node) const;

  // Check that an estimate is available for every OP node in graph.
  void CheckInitialized(const Graph& graph) const;

  // Records the maximum size in bytes and optionally the corresponding shape of
  // the tensor generated by "output_slot" of "node". If
  void RecordMaxMemorySize(const Node* node, int output_slot, Bytes bytes,
                           const TensorShapeProto& tensor_shape,
                           const DataType& dtype);

  // Returns the maximum size in bytes of the tensor generated by "output_slot"
  // of "node".
  Bytes MaxMemorySize(const Node* node, int output_slot) const;

  // Returns the shape corresponding to the largest memory size of the tensor
  // generated by "output_slot" of "node".
  const TensorShapeProto& MaxMemoryShape(const Node* node,
                                         int output_slot) const;

  // Returns the shape corresponding to the largest memory size of the tensor
  // generated by "output_slot" of "node".
  DataType MaxMemoryType(const Node* node, int output_slot) const;

  // Returns the size in bytes of temporary memory consumed by "node".
  Bytes TempMemorySize(const Node* node) const;

  // Returns the size of persistent memory allocated by "node".
  Bytes PersistentMemorySize(const Node* node) const;

  // Records memory stats such as temp momory and persistent memory.
  void RecordMemoryStats(const Node* node, const MemoryStats& memory_stats);

  // Records the maximum execution time (in microseconds) of "node".
  void RecordMaxExecutionTime(const Node* node, Microseconds time);

  // Returns the maximum execution time (in microseconds) of "node".
  Microseconds MaxExecutionTime(const Node* node) const;

  // Record the unique id of the tensor generated by "output_slot" of "node".
  // Any other tensor sharing the same id will be an alias, i.e. it will share
  // the same underlying memory storage area.
  void RecordAllocationId(const Node* node, int output_slot, int64_t alloc_id);

  // Return the unique id of the tensor generated by "output_slot" of "node".
  int64_t AllocationId(const Node* node, int output_slot) const;

  bool IsPersistentTensor(const Node* node, int64_t alloc_id) const;

  // Helper routines to encapsulate static estimation heuristics

  // Compute an estimate of the time to copy "b" bytes over the network,
  // given a fixed cost of "network_latency_millis" milliseconds and
  // an estimated bandwidth of "estimated_gbps" gigabits per second (note that
  // this value is in gigabits, not gigabytes).
  static Microseconds CopyTimeEstimate(Bytes b, double network_latency_millis,
                                       double estimated_gbps);
  static Microseconds ComputationTimeEstimate(int64_t mathops);

  // Add this CostModel into the CostGraphDef.
  void AddToCostGraphDef(const Graph* graph, CostGraphDef* cost_graph) const;

  // Write the contents of the CostModel to the INFO log.
  void WriteSummaryToLog() const;

  // Increment the times that the cost model is updated.
  void IncrementUpdateTimes();

  // Get the times that the cost model is updated.
  int32 GetUpdateTimes() const;

 private:
  static Bytes MinTensorMemoryUsage(const TensorShapeProto& tensor_shape,
                                    const DataType& dtype);

  const bool is_global_;

  // Resizes vectors so that they are large enough for "id" and id's outputs.
  void Ensure(int id, int num_outputs);

  // Nodes and Edges whose count is < this value
  // get type/byte estimates of 0.
  int32 min_count_ = 0;

  // The number of times the cost model is updated.
  int32 update_times_ = 0;

  // Number of times each Node has been executed.
  std::vector<int32> count_;
  // Cumulative execution time.
  std::vector<Microseconds> time_;
  // Cumulative Bytes output on each channel.
  std::vector<gtl::InlinedVector<Bytes, 2>> slot_bytes_;

  // Maximum execution time
  std::vector<Microseconds> max_exec_time_;

  // Maximum memory usage
  struct MemUsage {
    MemUsage() : temp_memory_size(0), persistent_memory_size(0) {}

    // TODO(yuefengz): temp_memory_size is not being used, remove it.
    Bytes temp_memory_size;
    Bytes persistent_memory_size;

    gtl::InlinedVector<Bytes, 2> output_port_mem;
    gtl::InlinedVector<TensorShapeProto, 2> output_port_shape;
    gtl::InlinedVector<DataType, 2> output_port_type;
  };
  std::vector<MemUsage> max_mem_usage_;

  std::vector<gtl::InlinedVector<int64_t, 2>> output_port_alloc_ids_;

  std::set<int64_t> persistent_alloc_ids_;
  std::map<string, std::set<int64_t>> persistent_alloc_ids_by_devices_;

  TensorShapeProto unknown_shape_;

  TF_DISALLOW_COPY_AND_ASSIGN(CostModel);
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_GRAPH_COSTMODEL_H_
