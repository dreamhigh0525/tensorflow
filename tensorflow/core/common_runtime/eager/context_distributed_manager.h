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

#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_EAGER_CONTEXT_DISTRIBUTED_MANAGER_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_EAGER_CONTEXT_DISTRIBUTED_MANAGER_H_

#include "tensorflow/c/eager/immediate_execution_context.h"
#include "tensorflow/c/eager/immediate_execution_distributed_manager.h"
#include "tensorflow/core/common_runtime/eager/context.h"
#include "tensorflow/core/platform/status.h"

#if !defined(IS_MOBILE_PLATFORM)
#include "tensorflow/core/distributed_runtime/coordination/coordination_service.h"
#include "tensorflow/core/distributed_runtime/coordination/coordination_service_agent.h"
#endif  // !IS_MOBILE_PLATFORM

namespace tensorflow {
#if !defined(IS_MOBILE_PLATFORM)
class EagerContext;
class ServerDef;

class EagerContextDistributedManager
    : public ImmediateExecutionDistributedManager {
 public:
  explicit EagerContextDistributedManager(EagerContext* context)
      : context_(context),
        step_id_(0),
        coordination_service_agent_(CreateCoordinationServiceAgent()) {}

  Status SetOrUpdateServerDef(const ServerDef& server_def, bool reset_context,
                              int keep_alive_secs) override;

  Status EnableCollectiveOps(const ServerDef& server_def) override;

  Status EnableCoordinationService(const std::string& service_type,
                                   const WorkerEnv* worker_env,
                                   const ServerDef& server_def,
                                   WorkerCacheInterface* worker_cache) override;

  Status CheckRemoteAlive(const std::string& remote_task_name,
                          bool* is_alive) override;

  CoordinationServiceAgent* GetCoordinationServiceAgent() override {
    return coordination_service_agent_.get();
  }

  // An atomic operation for getting the next (monotonically increasing)
  // step_id. It is the caller's responsibility to make sure the threads use the
  // same step_id for rendezvous send/recv.
  int64_t GetNextStepId() override { return ++step_id_; }

  int64_t step_id() override { return step_id_; }

 private:
  EagerContext* context_;
  std::atomic<int64_t> step_id_;
  std::unique_ptr<CoordinationServiceInterface> coordination_service_;
  std::unique_ptr<CoordinationServiceAgent> coordination_service_agent_;
};
#endif  // !IS_MOBILE_PLATFORM
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_EAGER_CONTEXT_DISTRIBUTED_MANAGER_H_
