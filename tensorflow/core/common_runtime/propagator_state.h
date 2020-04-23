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
#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_PROPAGATOR_STATE_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_PROPAGATOR_STATE_H_

#include <vector>

#include "tensorflow/core/common_runtime/entry.h"
#include "tensorflow/core/common_runtime/immutable_executor_state.h"
#include "tensorflow/core/common_runtime/pending_counts.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/control_flow.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

typedef gtl::InlinedVector<AllocatorAttributes, 4> AllocatorAttributeVec;

// Represents the ephemeral "edge state" associated with one invocation of
// `Executor::Run()`.
//
// `PropagatorState` is responsible for propagating values along dataflow
// edges in a TensorFlow graph and determining which nodes are runnable. The
// executor primarily updates `PropagatorState` by calling `PropagateOutputs()`
// after processing a node, and `PropagatorState` dispatches `TaggedNode`s by
// adding them to a `TaggedNodeSeq`.
class PropagatorState {
 public:
  PropagatorState(const ImmutableExecutorState& immutable_state, int64 step_id);
  ~PropagatorState();

 private:
  // Forward declaration so that `TaggedNode` can include a `FrameState*`.
  struct FrameState;

 public:
  // A `TaggedNode` corresponds to a single invocation of a node's kernel,
  // and it is created when the kernel becomes runnable (in a particular
  // iteration of a particular frame).
  struct TaggedNode {
    const NodeItem* node_item;
    FrameState* input_frame;
    int64 input_iter;
    bool is_dead;

    TaggedNode() = default;
    TaggedNode(const NodeItem* node_item, FrameState* in_frame, int64 in_iter,
               bool dead)
        : node_item(node_item),
          input_frame(in_frame),
          input_iter(in_iter),
          is_dead(dead) {}

    const NodeItem& get_node_item() const { return *node_item; }

    bool get_is_dead() const { return is_dead; }
    int64 get_iter_num() const { return input_iter; }
  };

  // A drop-in replacement for std::deque<TaggedNode>.  We typically don't
  // have that many nodes in the ready queue, so we just use a vector and
  // don't free up memory from the queue as we consume nodes.
  class TaggedNodeReadyQueue {
   public:
    TaggedNodeReadyQueue() : front_index_(0) {}

    void push_back(const TaggedNode& node) { ready_.push_back(node); }
    TaggedNode front() const {
      DCHECK_LT(front_index_, ready_.size());
      return ready_[front_index_];
    }
    void pop_front() {
      DCHECK_LT(front_index_, ready_.size());
      front_index_++;
      if ((front_index_ == ready_.size()) || (front_index_ > kSpillThreshold)) {
        if (front_index_ == ready_.size()) {
          ready_.clear();
        } else {
          // Lots of unused entries at beginning of vector: move everything
          // down to start of vector.
          ready_.erase(ready_.begin(), ready_.begin() + front_index_);
        }
        front_index_ = 0;
      }
    }
    bool empty() const { return ready_.empty(); }

   private:
    // TODO(b/152925936): Re-evaluate these constants with current usage
    // patterns.
    static constexpr int kSpillThreshold = 16384;
    gtl::InlinedVector<TaggedNode, 16> ready_;
    int front_index_;
  };

  // TODO(b/152925936): Re-evaluate this constant with current usage patterns.
  typedef gtl::InlinedVector<TaggedNode, 8> TaggedNodeSeq;

 private:
  struct IterationState {
    explicit IterationState(const PendingCounts* pending_counts,
                            int total_input_tensors)
        : input_tensors(new Entry[total_input_tensors]),
          outstanding_ops(0),
          outstanding_frame_count(0),
          counts(*pending_counts) {  // Initialize with copy of *pending_counts
    }

    // The state of an iteration.

    // One copy per iteration. For iteration k, i-th node's j-th input is in
    // input_tensors[k][immutable_state_.nodes[i].input_start + j]. An entry is
    // either a tensor pointer (pass-by-reference) or a tensor (pass-by-value).
    //
    // NOTE: No need to protect input_tensors[i] by any locks because it
    // is resized once. Each element of tensors_ is written once by the
    // source node of an edge and is cleared by the destination of the same
    // edge. The latter node is never run concurrently with the former node.
    Entry* input_tensors;

    // The number of outstanding ops for each iteration.
    size_t outstanding_ops;

    // The number of outstanding frames for each iteration.
    int outstanding_frame_count;
    int pending(PendingCounts::Handle h) { return counts.pending(h); }
    int decrement_pending(PendingCounts::Handle h, int v) {
      return counts.decrement_pending(h, v);
    }
    // Mark a merge node as live
    // REQUIRES: Node corresponding to "h" is a merge node
    void mark_live(PendingCounts::Handle h) { counts.mark_live(h); }
    // Mark a node to show that processing has started.
    void mark_started(PendingCounts::Handle h) { counts.mark_started(h); }
    // Mark a node to show that processing has completed.
    void mark_completed(PendingCounts::Handle h) { counts.mark_completed(h); }
    PendingCounts::NodeState node_state(PendingCounts::Handle h) {
      return counts.node_state(h);
    }

    int dead_count(PendingCounts::Handle h) { return counts.dead_count(h); }
    void increment_dead_count(PendingCounts::Handle h) {
      counts.increment_dead_count(h);
    }
    PendingCounts::AdjustResult adjust_for_activation(PendingCounts::Handle h,
                                                      bool increment_dead) {
      return counts.adjust_for_activation(h, increment_dead);
    }

    ~IterationState() { delete[] input_tensors; }

   private:
    PendingCounts counts;
  };

  struct FrameState {
    explicit FrameState(const ImmutableExecutorState& immutable_state,
                        int parallel_iters)
        : immutable_state(immutable_state),
          max_parallel_iterations(parallel_iters),
          num_outstanding_iterations(1),
          iterations(parallel_iters + 1),
          iterations_raw(iterations.data()) {}

    // A new frame is created for each loop. Execution starts at iteration 0.
    // When a value at iteration 0 passes through a NextIteration node,
    // iteration 1 is created and starts running. Note that iteration 0 may
    // still be running so multiple iterations may run in parallel. The
    // frame maintains the state of iterations in several data structures
    // such as pending_count and input_tensors. When iteration 0 completes,
    // we garbage collect the state of iteration 0.
    //
    // A frame instance is considered "done" and can be garbage collected
    // if all its inputs have entered and all its iterations are "done".
    //
    // A frame manages the live iterations of an iterative computation.
    // Iteration i is considered "done" when there are no outstanding ops,
    // frames at iteration i are done, all recvs for this iteration are
    // completed, and iteration i-1 is done. For iteration 0, we instead
    // wait for there to be no more pending inputs of the frame.
    //
    // Frames and iterations are garbage collected once they are done.
    // The state we need to keep around is highly dependent on the
    // parallelism enabled by the scheduler. We may want to have the
    // scheduler dynamically control the outstanding number of live
    // parallel frames and iterations. To reduce the state space, the
    // scheduler might want to schedule ops in inner frames first and
    // lower iterations first.
    //
    // This frame state is mostly initialized lazily on demand so we
    // don't introduce unnecessary overhead.

    // The immutable state of the executor the frame is in.
    const ImmutableExecutorState& immutable_state;

    // The name of this frame, which is the concatenation of its parent
    // frame name, the iteration of the parent frame when this frame was
    // created, and the value of the attr 'frame_name'.
    string frame_name;

    // The unique id for this frame. Generated by fingerprinting
    // frame_name.
    uint64 frame_id;

    // The iteration id of its parent frame when this frame is created.
    // -1 if there is no parent frame. The frame_name/parent_iter pair
    // uniquely identifies this FrameState.
    int64 parent_iter = -1;

    // The FrameState of its parent frame.
    FrameState* parent_frame = nullptr;

    // The maximum allowed number of parallel iterations.
    const int max_parallel_iterations;

    // The number of inputs this frame is still waiting.
    int num_pending_inputs = 0;

    // The highest iteration number we have reached so far in this frame.
    int64 iteration_count TF_GUARDED_BY(mu) = 0;

    // The number of outstanding iterations.
    int num_outstanding_iterations TF_GUARDED_BY(mu) = 1;

   private:
    // The active iteration states of this frame.
    gtl::InlinedVector<IterationState*, 12> iterations;
    IterationState** const iterations_raw TF_GUARDED_BY(mu);
    IterationState* iterations_first TF_GUARDED_BY(mu);

   public:
    // The NextIteration nodes to enter a new iteration. If the number of
    // outstanding iterations reaches the limit, we will defer the start of
    // the next iteration until the number of outstanding iterations falls
    // below the limit.
    std::vector<std::pair<const NodeItem*, Entry>> next_iter_roots
        TF_GUARDED_BY(mu);

    // The values of the loop invariants for this loop. They are added into
    // this list as they "enter" the frame. When a loop invariant enters,
    // we make it available to all active iterations. When the frame starts
    // a new iteration, we make all the current loop invariants available
    // to the new iteration.
    std::vector<std::pair<const NodeItem*, Entry>> inv_values TF_GUARDED_BY(mu);

    // The list of dead exit node items for the current highest iteration. We
    // will only "execute" the dead exits of the final iteration.
    std::vector<const NodeItem*> dead_exits TF_GUARDED_BY(mu);

    // Static information specific to this frame.
    PendingCounts* pending_counts = nullptr;
    int total_input_tensors = 0;
    std::vector<const NodeItem*>* nodes = nullptr;

    // Lock ordering: ExecutorState.mu_ < mu;
    // during structured traversal: parent_frame->mu < mu.
    mutex mu;

    void InitializeFrameInfo(const string& enter_name);

    inline IterationState* GetIteration(int64 iter)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu) {
      if (TF_PREDICT_TRUE(iter == 0)) {
        return iterations_first;
      } else {
        size_t index = iter % (max_parallel_iterations + 1);
        return iterations_raw[index];
      }
    }

    void SetIteration(int64 iter, IterationState* state);

    // Decrement the outstanding op count and clean up the iterations in the
    // frame. Return true iff the execution of the frame is done.
    bool DecrementOutstandingOps(int64 iter, TaggedNodeSeq* ready);

    // Decrement the outstanding op count and clean up the iterations in the
    // frame. Return true iff the execution of the frame is done.
    bool DecrementOutstandingOpsLocked(int64 iter, TaggedNodeSeq* ready);

    // Returns true if the computation in the frame is completed.
    bool IsFrameDone();

    // Returns true if the iteration of the frame is completed.
    bool IsIterationDone(int64 iter) TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Increments the iteration id. If this is a new iteration, initialize it.
    void IncrementIteration(TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Activate all the deferred NextIteration nodes in a new iteration.
    void ActivateNexts(int64 iter, TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Activate all the current loop invariants in a new iteration.
    void ActivateLoopInvs(int64 iter, TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Add a new loop invariant and make it available to all active
    // iterations.
    void AddLoopInv(const NodeItem* item, const Entry& entry,
                    TaggedNodeSeq* ready) TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Activate the successors of a node. Contents of *outputs are left in an
    // indeterminate state after returning from this method.
    void ActivateNodes(const NodeItem* item, const bool is_dead, int64 iter,
                       EntryVector* outputs, TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    // Cleanup iterations of this frame starting from iteration iter.
    bool CleanupIterations(int64 iter, TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    void DumpIterationState(PropagatorState* parent) {
      mutex_lock l(mu);
      for (IterationState* iteration : iterations) {
        if (iteration) {
          LOG(WARNING) << "  Iteration:";
          parent->DumpIterationState(this, iteration);
        }
      }
    }

    ~FrameState() {
      for (size_t i = 0; i < iterations.size(); ++i) {
        delete iterations[i];
        iterations[i] = nullptr;
      }
    }

   private:
    // REQUIRES: `!item->is_any_consumer_merge_or_control_trigger`.
    void ActivateNodesFastPath(const NodeItem* item, const bool is_dead,
                               int64 iter, EntryVector* outputs,
                               TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);

    void ActivateNodesSlowPath(const NodeItem* item, const bool is_dead,
                               int64 iter, EntryVector* outputs,
                               TaggedNodeSeq* ready)
        TF_EXCLUSIVE_LOCKS_REQUIRED(mu);
  };

 public:
  // Creates and adds a `TaggedNode` for each node in `roots` to `*ready`.
  void ActivateRoots(gtl::ArraySlice<const NodeItem*> roots,
                     TaggedNodeSeq* ready);

  // After processing the outputs, propagates the outputs to their dsts.
  // Contents of *outputs are left in an indeterminate state after
  // returning from this method.
  void PropagateOutputs(const TaggedNode& tagged_node, EntryVector* outputs,
                        TaggedNodeSeq* ready);

  // Returns an array of `Entry` objects corresponding to the inputs of
  // `tagged_node`.
  //
  // NOTE: Thread safety analysis is disabled on this method, because the
  // underlying `IterationState` and its array of `input_tensors` retain the
  // same address while the iteration is live.
  Entry* GetInputTensors(const TaggedNode& tagged_node) const
      TF_NO_THREAD_SAFETY_ANALYSIS {
    return tagged_node.input_frame->GetIteration(tagged_node.input_iter)
               ->input_tensors +
           tagged_node.node_item->input_start;
  }

  FrameAndIter GetFrameAndIter(const TaggedNode& tagged_node) const {
    return {tagged_node.input_frame->frame_id, tagged_node.input_iter};
  }

  // Provide debugging output of the state of the executor.
  void DumpState();

  // For debugging/logging only.
  void MaybeMarkStarted(const TaggedNode& tagged_node) {
    // TODO(misard) Replace with a finer-grain enabling flag once we add better
    // optional debugging support.
    if (TF_PREDICT_FALSE(vlog_) && VLOG_IS_ON(1)) {
      mutex_lock l(tagged_node.input_frame->mu);
      tagged_node.input_frame->GetIteration(tagged_node.input_iter)
          ->mark_started(
              immutable_state_.pending_ids()[tagged_node.node_item->node_id]);
    }
  }

  void MaybeMarkCompleted(const TaggedNode& tagged_node) {
    // TODO(misard) Replace with a finer-grain enabling flag once we add better
    // optional debugging support.
    if (TF_PREDICT_FALSE(vlog_) && VLOG_IS_ON(1)) {
      mutex_lock l(tagged_node.input_frame->mu);
      tagged_node.input_frame->GetIteration(tagged_node.input_iter)
          ->mark_completed(
              immutable_state_.pending_ids()[tagged_node.node_item->node_id]);
    }
  }

 private:
  // Find an existing or create a new child frame in the frame 'frame' at
  // iteration 'iter'.
  void FindOrCreateChildFrame(FrameState* frame, int64 iter,
                              const NodeItem& node_item, FrameState** child);

  // Delete a frame. Called when the frame is done.
  void DeleteFrame(FrameState* frame, TaggedNodeSeq* ready);

  // Cleanup frames and iterations starting from frame/iter. Called when
  // a child frame is done.
  void CleanupFramesIterations(FrameState* frame, int64 iter,
                               TaggedNodeSeq* ready);

  // Provide debugging output about an outstanding iteration in the executor.
  void DumpIterationState(const FrameState* frame, IterationState* iteration);

  const ImmutableExecutorState& immutable_state_;
  const int64 step_id_;
  const bool vlog_;

  mutex mu_;

  // The root frame in which the execution of this step is started.
  FrameState* root_frame_;

  // Mapping from frame name to outstanding frames. A new frame is created
  // at some iteration of an active frame. So the unique key for the new
  // child frame is composed of the name of the parent frame, the iteration
  // number at which the parent frame is creating the new frame, and the
  // name of the new frame from nodedef.
  gtl::FlatMap<string, FrameState*> outstanding_frames_ TF_GUARDED_BY(mu_);

  TF_DISALLOW_COPY_AND_ASSIGN(PropagatorState);
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_PROPAGATOR_STATE_H_
