/* Copyright 2015 Google Inc. All Rights Reserved.

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

#ifndef TENSORFLOW_COMMON_RUNTIME_GPU_GPU_EVENT_MGR_H_
#define TENSORFLOW_COMMON_RUNTIME_GPU_GPU_EVENT_MGR_H_

#include <deque>
#include <vector>
#include "tensorflow/stream_executor/stream.h"
#include "tensorflow/core/lib/core/notification.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/platform/port.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/public/tensor.h"

namespace perftools {
namespace gputools {
class Event;
class Stream;
class StreamExecutor;
}  // namespace gputools
}  // namespace perftools

namespace tensorflow {

// An object to keep track of pending Events in the StreamExecutor streams
// and associated Tensors that cannot safely be deleted until the associated
// Events are recorded.
class EventMgr {
 public:
  explicit EventMgr(perftools::gputools::StreamExecutor* se);

  ~EventMgr();

  // Takes ownership of *tensors and deletes it as soon as all events
  // currently enqueued on *stream have completed.
  inline void ThenDeleteTensors(perftools::gputools::Stream* stream,
                                std::vector<Tensor>* tensors) {
    ToFreeVector to_free;
    ::perftools::gputools::Event* e;
    {
      mutex_lock l(mu_);
      QueueTensors(stream, tensors, &e);
      PollEvents(false, &to_free);
    }
    stream->ThenRecordEvent(e);
    FreeMemory(to_free);
  }

  struct BufRec {
    Allocator* alloc;
    void* buf;
  };

  // Takes ownership of *bufrec.buf and calls bufrec.alloc->DeallocateRaw()
  // on it as soon as all events currently enqueued on *stream have completed.
  inline void ThenDeleteBuffer(perftools::gputools::Stream* stream,
                               BufRec bufrec) {
    ToFreeVector to_free;
    ::perftools::gputools::Event* e;
    {
      mutex_lock l(mu_);
      QueueBuffer(stream, bufrec, &e);
      PollEvents(false, &to_free);
    }
    stream->ThenRecordEvent(e);
    FreeMemory(to_free);
  }

  inline void ThenExecute(perftools::gputools::Stream* stream,
                          std::function<void()> func) {
    ToFreeVector to_free;
    ::perftools::gputools::Event* e;
    {
      mutex_lock l(mu_);
      QueueFunc(stream, func, &e);
      PollEvents(false, &to_free);
    }
    stream->ThenRecordEvent(e);
    FreeMemory(to_free);
  }

 private:
  friend class TEST_EventMgrHelper;
  mutex mu_;
  perftools::gputools::StreamExecutor* exec_;

  struct InUse {
    perftools::gputools::Event* event;
    std::vector<Tensor>* mem;
    BufRec bufrec;
    std::function<void()> func;
  };

  typedef gtl::InlinedVector<InUse, 4> ToFreeVector;

  void FreeMemory(const ToFreeVector& to_free) {
    for (const auto& iu : to_free) {
      delete iu.mem;
      if (iu.bufrec.buf) iu.bufrec.alloc->DeallocateRaw(iu.bufrec.buf);
      // The function must be called in another thread.
      if (iu.func != nullptr) threadpool_.Schedule(iu.func);
    }
  }

  // Stream-enqueue an unused Event and save with it a collection of
  // Tensors and/or a BufRec to be deleted only after the Event
  // records.
  void QueueInUse(perftools::gputools::Stream* stream, InUse in_use,
                  ::perftools::gputools::Event** e)
      EXCLUSIVE_LOCKS_REQUIRED(mu_);

  void QueueTensors(perftools::gputools::Stream* stream,
                    std::vector<Tensor>* tensors,
                    ::perftools::gputools::Event** e)
      EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    QueueInUse(stream, {nullptr, tensors, BufRec(), nullptr}, e);
  }

  void QueueBuffer(perftools::gputools::Stream* stream, BufRec bufrec,
                   ::perftools::gputools::Event** e)
      EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    QueueInUse(stream, {nullptr, nullptr, bufrec, nullptr}, e);
  }

  void QueueFunc(perftools::gputools::Stream* stream,
                 std::function<void()> func, ::perftools::gputools::Event** e)
      EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    QueueInUse(stream, {nullptr, nullptr, BufRec(), func}, e);
  }

  // This function should be called at roughly the same tempo as
  // QueueTensors() to check whether pending events have recorded,
  // and then retire them.  It appends InUse elements that need cleanup
  // to "*to_free".  The caller should call FreeMemory(to_free)
  // when this returns.
  void PollEvents(bool is_dedicated_poller, ToFreeVector* to_free)
      EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // An internal polling loop that runs at a low frequency to clear
  // straggler Events.
  void PollLoop();

  // A stack of unused events
  std::vector<perftools::gputools::Event*> free_events_ GUARDED_BY(mu_);

  // A FIFO queue of InUse events and associated tensors.
  std::deque<InUse> used_events_ GUARDED_BY(mu_);

  Notification stop_polling_;
  Notification polling_stopped_;

  // The main PollLoop for the event manager runs in this threadpool.
  thread::ThreadPool threadpool_;
};

}  // namespace tensorflow
#endif  // TENSORFLOW_COMMON_RUNTIME_GPU_GPU_EVENT_MGR_H_
