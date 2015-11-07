#ifndef TENSORFLOW_FRAMEWORK_QUEUE_INTERFACE_H_
#define TENSORFLOW_FRAMEWORK_QUEUE_INTERFACE_H_

#include <string>
#include <vector>

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/platform/port.h"
#include "tensorflow/core/public/tensor.h"

namespace tensorflow {

// All implementations must be thread-safe.
class QueueInterface : public ResourceBase {
 public:
  typedef std::vector<Tensor> Tuple;
  typedef AsyncOpKernel::DoneCallback DoneCallback;
  typedef std::function<void(const Tuple&)> CallbackWithTuple;

  virtual Status ValidateTuple(const Tuple& tuple) = 0;
  virtual Status ValidateManyTuple(const Tuple& tuple) = 0;

  // Stashes a function object for future execution, that will eventually
  // enqueue the tuple of tensors into the queue, and returns immediately. The
  // function object is guaranteed to call 'callback'.
  virtual void TryEnqueue(const Tuple& tuple, OpKernelContext* ctx,
                          DoneCallback callback) = 0;

  // Same as above, but the component tensors are sliced along the 0th dimension
  // to make multiple queue-element components.
  virtual void TryEnqueueMany(const Tuple& tuple, OpKernelContext* ctx,
                              DoneCallback callback) = 0;

  // Stashes a function object for future execution, that will eventually
  // dequeue an element from the queue and call 'callback' with that tuple
  // element as argument.
  virtual void TryDequeue(OpKernelContext* ctx, CallbackWithTuple callback) = 0;

  // Same as above, but the stashed function object will attempt to dequeue
  // num_elements items.
  virtual void TryDequeueMany(int num_elements, OpKernelContext* ctx,
                              CallbackWithTuple callback) = 0;

  // Signals that no more elements will be enqueued, and optionally
  // cancels pending Enqueue(Many) operations.
  //
  // After calling this function, subsequent calls to Enqueue(Many)
  // will fail. If `cancel_pending_enqueues` is true, all pending
  // calls to Enqueue(Many) will fail as well.
  //
  // After calling this function, all current and subsequent calls to
  // Dequeue(Many) will fail instead of blocking (though they may
  // succeed if they can be satisfied by the elements in the queue at
  // the time it was closed).
  virtual void Close(OpKernelContext* ctx, bool cancel_pending_enqueues,
                     DoneCallback callback) = 0;

  // Assuming *this represents a shared queue, verify that it matches
  // another instantiation indicated by node_def.
  virtual Status MatchesNodeDef(const NodeDef& node_def) = 0;

  // Returns the number of elements in the queue.
  virtual int32 size() = 0;

  virtual const DataTypeVector& component_dtypes() const = 0;

  string DebugString() override { return "A queue"; }

 protected:
  virtual ~QueueInterface() {}
};

}  // namespace tensorflow

#endif  // TENSORFLOW_FRAMEWORK_QUEUE_INTERFACE_H_
