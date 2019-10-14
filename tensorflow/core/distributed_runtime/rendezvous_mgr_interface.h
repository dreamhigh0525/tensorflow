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

#ifndef TENSORFLOW_CORE_DISTRIBUTED_RUNTIME_RENDEZVOUS_MGR_INTERFACE_H_
#define TENSORFLOW_CORE_DISTRIBUTED_RUNTIME_RENDEZVOUS_MGR_INTERFACE_H_

#include <string>

#include "tensorflow/core/distributed_runtime/worker_env.h"
#include "tensorflow/core/framework/rendezvous.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {

class WorkerSession;

// RemoteRendezvous follow a 2-part initialization. First the objects are
// constructed. Eventually, they will be initialized. Clients of the
// RendezvousMgrInterface must guarantee to call Initialize on the returned
// RemoteRendezvous eventually.
//
// Partially initialized RemoteRendezvous must respect the Rendezvous interface
// (i.e. Send() must never block), however implementations are not expected to
// actually perform the underlying operations until after the RemoteRendezvous
// has been Initialize'd.
class RemoteRendezvous : public Rendezvous {
 public:
  // Fully construct the RemoteRendezvous.
  virtual Status Initialize(WorkerSession* session) = 0;
};

// RendezvousMgr keeps track of a set of local rendezvous instances.
// All tensors sent by this worker are buffered in a RendezvousMgr
// until the tensor is received.  Each global unique "step_id"
// corresponds to one local rendezvous instance managed by a
// RendezvousMgr.
//
// E.g.,
//   Rendezvous* rendez = worker_env->rendezvous_mgr->Find(0x8935);
//   fork execution of an graph executor using "rendez"  on thread 1;
//   fork execution of another graph executor using "rendez" on thread 2;
//   ...
//   join threads 1 and 2;
//
// In the example above, execution in thread 1 and 2 communicates with
// each other by send/recv operations through the "rend".
//
// Tensors sent and recved through rendezvous managed by this
// RendezvousMgr must have keys generated by Rendezvous::CreateKey.
class RendezvousMgrInterface {
 public:
  RendezvousMgrInterface() {}
  virtual ~RendezvousMgrInterface() {}

  // Returns Rendezvous supporting send and recv among workers in the
  // "step_id".  The caller takes ownership of one reference on the
  // returned Rendezvous instance.
  //
  // Note: the caller must guarantee to eventually call Initialize on the
  // returned RemoteRendezvous
  virtual RemoteRendezvous* Find(int64 step_id) = 0;

  // Finds the local rendezvous instance for the "step_id".  Runs
  // "done" when the tensor for "key" is produced or an error occurs.
  //
  // This method is used by the rpc handler of RecvTensor.
  virtual void RecvLocalAsync(int64 step_id,
                              const Rendezvous::ParsedKey& parsed,
                              Rendezvous::DoneCallback done) = 0;

  // Synchronous wrapper for RecvLocalAsync.
  virtual Status RecvLocal(int64 step_id, const Rendezvous::ParsedKey& parsed,
                           Tensor* val, bool* is_dead) = 0;

  // Removes rendezvous for "step_id".
  //
  // TODO(zhifengc): Have a background thread in worker that
  // periodically calls CleanupAll().
  virtual void Cleanup(int64 step_id) = 0;

  // Removes all rendezvous.
  virtual void CleanupAll() = 0;
};

}  // end namespace tensorflow

#endif  // TENSORFLOW_CORE_DISTRIBUTED_RUNTIME_RENDEZVOUS_MGR_INTERFACE_H_
