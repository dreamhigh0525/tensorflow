/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CORE_DISTRIBUTED_RUNTIME_BASE_RENDEZVOUS_MGR_H_
#define TENSORFLOW_CORE_DISTRIBUTED_RUNTIME_BASE_RENDEZVOUS_MGR_H_

#include <string>
#include <unordered_set>

#include "tensorflow/core/distributed_runtime/rendezvous_mgr_interface.h"
#include "tensorflow/core/distributed_runtime/worker_env.h"
#include "tensorflow/core/distributed_runtime/worker_session.h"
#include "tensorflow/core/framework/control_flow.h"
#include "tensorflow/core/framework/rendezvous.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/flatset.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/device_name_utils.h"

namespace tensorflow {

class BaseRemoteRendezvous;
class BaseRecvTensorCall;

// RendezvousMgr keeps track of a set of local rendezvous instances.
// All tensors sent by this worker are buffered in a RendezvousMgr
// until the tensor is received.  Each global unique "step_id"
// corresponds to one local rendezvous instance managed by a
// RendezvousMgr.
//
// E.g.,
//   Rendezvous* rendez = worker_env->rendezvous_mgr->Find(0x8935);
//   fork execution of a graph executor using "rendez" on thread 1;
//   fork execution of another graph executor using "rendez" on thread 2;
//   ...
//   join threads 1 and 2;
//
// In the example above, execution in thread 1 and 2 communicates with
// each other by send/recv operations through `rendez`.
//
// Tensors sent and received through a rendezvous managed by this
// RendezvousMgr must have keys generated by Rendezvous::CreateKey().
class BaseRendezvousMgr : public RendezvousMgrInterface {
 public:
  explicit BaseRendezvousMgr(const WorkerEnv* worker_env);

  ~BaseRendezvousMgr() override;

  // Returns Rendezvous supporting send and recv among workers in the
  // "step_id".  The caller takes ownership of one reference on the
  // returned Rendezvous instance.
  //
  // Note: the caller must guarantee to eventually call Initialize on the
  // returned RemoteRendezvous
  RemoteRendezvous* Find(int64 step_id) override;

  // Finds the local rendezvous instance for the "step_id".  Runs
  // "done" when the tensor for "key" is produced or an error occurs.
  //
  // This method is used by the rpc handler of RecvTensor.
  void RecvLocalAsync(int64 step_id, const Rendezvous::ParsedKey& parsed,
                      Rendezvous::DoneCallback done) override;

  // Synchronous wrapper for RecvLocalAsync.
  Status RecvLocal(int64 step_id, const Rendezvous::ParsedKey& parsed,
                   Tensor* val, bool* is_dead) override;

  // Removes rendezvous for "step_id".
  //
  // TODO(zhifengc): Have a background thread in worker that
  // periodically calls CleanupAll().
  void Cleanup(int64 step_id) override;

  // Removed all rendezvous.
  void CleanupAll() override;

 protected:
  virtual BaseRemoteRendezvous* Create(int64 step_id,
                                       const WorkerEnv* worker_env) = 0;

 private:
  // Maps step_id to rendezvous.
  typedef gtl::FlatMap<int64, BaseRemoteRendezvous*> Table;

  // Not owned.
  const WorkerEnv* const worker_env_;

  mutex mu_;
  Table table_ GUARDED_BY(mu_);

  BaseRemoteRendezvous* FindOrCreate(int64 step_id);

  TF_DISALLOW_COPY_AND_ASSIGN(BaseRendezvousMgr);
};

// RemoteRendezvous is a Rendezvous which can handle either
// the producer or consumer being in a remote process.
//
// Buffering of Tensor values is delegated to a "local" Rendezvous
// obtained from NewLocalRendezvous().  This class just adds
// functionality to coordinate with remote workers.
class BaseRemoteRendezvous : public RemoteRendezvous {
 public:
  BaseRemoteRendezvous(const WorkerEnv* env, int64 step_id);

  // Upgrades the BaseRemoteRendezvous to full initialization.
  Status Initialize(WorkerSession* session) override;

  // Forwards to local_, where the Tensor "val" will be buffered and
  // any waiting callback stored.
  Status Send(const ParsedKey& key, const Rendezvous::Args& args,
              const Tensor& val, const bool is_dead) override;

  // This method is called only by the RecvOp.  It tests to see
  // whether the value will be produced by a local or remote device
  // and handles accordingly.  In the local case it forwards to
  // local_, in the remote case it initiates an RPC request.
  void RecvAsync(const ParsedKey& key, const Rendezvous::Args& args,
                 DoneCallback done) override;

  void StartAbort(const Status& status) override;

  // This method is called only by the local Worker, forwarded through
  // the same method on RendezvousMgr.  This occurs when the Worker
  // has received a RecvTensor request, either locally or over the
  // network.  In either case it needs to retrieve a locally buffered
  // value from local_, and give it to its caller.
  //
  // Runs "done" as soon as the tensor for "parsed" is available or an error
  // is detected.
  //
  // REQUIRES: "parsed" is one that will be Saved into the local rendezvous.
  void RecvLocalAsync(const ParsedKey& parsed, DoneCallback done);

 protected:
  virtual void RecvFromRemoteAsync(const Rendezvous::ParsedKey& parsed,
                                   const Rendezvous::Args& args,
                                   DoneCallback done) = 0;

  // Returns true if "src" and "dst" are located in the same worker,
  // and hence may use a local rendezvous.
  virtual bool IsSameWorker(DeviceNameUtils::ParsedName src,
                            DeviceNameUtils::ParsedName dst);

  // If aborted, aborts "call". Otherwise, adds "call" into active_.
  void RegisterCall(BaseRecvTensorCall* call, const Rendezvous::Args& args);

  // Removes "call" from active_ if "call" is in active_.
  void DeregisterCall(BaseRecvTensorCall* call);

  WorkerSession* session();

  bool is_initialized();

  ~BaseRemoteRendezvous() override;

  const WorkerEnv* const env_;  // Not owned.
  const int64 step_id_;

 private:
  Rendezvous* local_;  // Owns a Ref on this object.

  mutable mutex mu_;

  // Status given by StartAbort() if any.
  Status status_ GUARDED_BY(mu_);
  WorkerSession* session_ GUARDED_BY(mu_);  // Not owned.

  // Data structures to handle calls when partially initialized.
  struct DeferredCall {
    const ParsedKey parsed;
    DoneCallback done;

    DeferredCall(const ParsedKey& parsed, DoneCallback done);
  };
  std::vector<DeferredCall> deferred_calls_ GUARDED_BY(mu_);

  typedef std::function<void()> InactiveCallback;

  // Active outstanding RecvTensor calls.
  std::unordered_map<BaseRecvTensorCall*, InactiveCallback> active_
      GUARDED_BY(mu_);

  bool is_initialized_locked() SHARED_LOCKS_REQUIRED(mu_) {
    return session_ != nullptr;
  }

  // If "is_src" is true, checks that the rendezvous key "parsed"'s
  // source is in this process. If "is_src" is false, checks that the
  // rendezvous key "parsed"'s destination is in this process.
  Status ValidateDevices(const Rendezvous::ParsedKey& parsed, bool is_src);

  // Callback handling the case when a rendezvous has been
  // accomplished in local_ and the consumer is local to this process.
  // Tensor "in" will be copied into "out". The key "parsed" encodes
  // the src and dst devices.
  void SameWorkerRecvDone(const Rendezvous::ParsedKey& parsed,
                          const Rendezvous::Args& in_args,
                          const Rendezvous::Args& out_args, const Tensor& in,
                          Tensor* out, StatusCallback done);

  // Must be called only if fully initialized.
  void RecvLocalAsyncInternal(const ParsedKey& parsed, DoneCallback done);

  TF_DISALLOW_COPY_AND_ASSIGN(BaseRemoteRendezvous);
};

class BaseRecvTensorCall {
 public:
  BaseRecvTensorCall() {}
  virtual ~BaseRecvTensorCall() {}

  virtual void Start(std::function<void()> recv_done) = 0;

  virtual void StartAbort(const Status& s) = 0;

  virtual Status status() const = 0;

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(BaseRecvTensorCall);
};

}  // end namespace tensorflow

#endif  // TENSORFLOW_CORE_DISTRIBUTED_RUNTIME_BASE_RENDEZVOUS_MGR_H_
