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

#include "tensorflow/core/data/service/worker_impl.h"

#include "grpcpp/create_channel.h"
#include "absl/memory/memory.h"
#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/core/data/dataset.pb.h"
#include "tensorflow/core/data/service/credentials_factory.h"
#include "tensorflow/core/data/service/dispatcher.grpc.pb.h"
#include "tensorflow/core/data/service/dispatcher.pb.h"
#include "tensorflow/core/data/service/grpc_util.h"
#include "tensorflow/core/data/standalone.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/io/zlib_outputbuffer.h"
#include "tensorflow/core/lib/monitoring/gauge.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/snappy.h"
#include "tensorflow/core/public/session_options.h"

namespace tensorflow {
namespace data {

const constexpr uint64 kRetryIntervalMicros = 5ull * 1000 * 1000;

namespace {
auto* tf_data_service_created =
    monitoring::Gauge<bool, 0>::New("/tensorflow/data/service/created",
                                    "Whether a tf.data service server "
                                    "has been created.");
}  // namespace

DataServiceWorkerImpl::DataServiceWorkerImpl(
    const experimental::WorkerConfig& config)
    : config_(config) {
  tf_data_service_created->GetCell()->Set(true);
}

DataServiceWorkerImpl::~DataServiceWorkerImpl() {
  mutex_lock l(mu_);
  cancelled_ = true;
  background_cv_.notify_one();
}

Status DataServiceWorkerImpl::Start(const std::string& worker_address) {
  VLOG(3) << "Starting tf.data service worker at address " << worker_address;
  worker_address_ = worker_address;

  std::unique_ptr<DispatcherService::Stub> dispatcher;
  TF_RETURN_IF_ERROR(MakeDispatcherStub(&dispatcher));

  Status s = Register(dispatcher.get());
  while (!s.ok()) {
    LOG(WARNING) << "Failed to register with dispatcher at "
                 << config_.dispatcher_address() << ": " << s;
    Env::Default()->SleepForMicroseconds(kRetryIntervalMicros);
    s = Register(dispatcher.get());
  }
  Thread* thread =
      Env::Default()->StartThread({}, "data-service-worker-background",
                                  [this, dispatcher = dispatcher.release()]() {
                                    BackgroundThread(dispatcher);
                                  });
  LOG(INFO) << "Worker registered with dispatcher running at "
            << config_.dispatcher_address();
  background_thread_.reset(thread);
  mutex_lock l(mu_);
  registered_ = true;
  return Status::OK();
}

Status DataServiceWorkerImpl::ProcessTask(const ProcessTaskRequest* request,
                                          ProcessTaskResponse* response) {
  mutex_lock l(mu_);
  const TaskDef& task = request->task();
  VLOG(3) << "Received request to process task " << task.task_id();
  return ProcessTaskInternal(task);
}

Status DataServiceWorkerImpl::ProcessTaskInternal(const TaskDef& task_def)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  VLOG(3) << "Received request to process task " << task_def.task_id();
  standalone::Dataset::Params params;
  std::unique_ptr<standalone::Dataset> dataset;
  TF_RETURN_IF_ERROR(standalone::Dataset::FromGraph(
      params, task_def.dataset().graph(), &dataset));

  std::unique_ptr<standalone::Iterator> iterator;
  TF_RETURN_IF_ERROR(dataset->MakeIterator(&iterator));

  if (tasks_.contains(task_def.task_id())) {
    return errors::AlreadyExists("A task with id ", task_def.task_id(),
                                 " already exists.");
  }
  Task& task = tasks_[task_def.task_id()];
  task.task_id = task_def.task_id();
  task.dataset = std::move(dataset);
  task.iterator = std::move(iterator);
  VLOG(3) << "Began processing for task " << task_def.task_id();
  return Status::OK();
}

Status DataServiceWorkerImpl::GetElement(const GetElementRequest* request,
                                         GetElementResponse* response) {
  VLOG(3) << "Received GetElement request for task " << request->task_id();
  bool end_of_sequence = false;
  std::vector<tensorflow::Tensor> outputs;
  {
    mutex_lock l(mu_);
    if (!registered_) {
      // We need to reject requests until the worker has registered with the
      // dispatcher, so that we don't return NOT_FOUND for tasks that the worker
      // had before preemption.
      return errors::Unavailable(
          "Worker has not yet registered with dispatcher.");
    }
    auto it = tasks_.find(request->task_id());
    if (it == tasks_.end()) {
      return errors::NotFound("DataServiceWorkerImpl::GetElement failed. ",
                              "Task id ", request->task_id(), " not found");
    }
    std::unique_ptr<standalone::Iterator>& iter = it->second.iterator;
    if (iter == nullptr) {
      VLOG(3) << "Task " << request->task_id() << " is already finished";
      response->set_end_of_sequence(true);
      return Status::OK();
    }
    TF_RETURN_IF_ERROR(iter->GetNext(&outputs, &end_of_sequence));
    if (end_of_sequence) {
      VLOG(3) << "Reached end_of_sequence for task " << request->task_id();
      // Release iterator memory and leave a null entry as a tombstone.
      iter.reset();
      pending_completed_tasks_.insert(request->task_id());
      background_cv_.notify_one();
    }
  }

  if (!end_of_sequence) {
    VLOG(3) << "Producing an element for task " << request->task_id();
    if (outputs.size() != 1) {
      return errors::FailedPrecondition(
          "Expected dataset to produce a single scalar variant tensor, but the "
          "dataset produced ",
          outputs.size(), " outputs");
    }
    if (outputs[0].dtype() != DT_VARIANT) {
      return errors::FailedPrecondition(
          "Expected dataset to produce a single scalar variant tensor, but "
          "the dataset produced a tensor with type ",
          DataTypeString(outputs[0].dtype()));
    }
    if (!TensorShapeUtils::IsScalar(outputs[0].shape())) {
      return errors::FailedPrecondition(
          "Expected dataset to produce a single scalar variant tensor, but "
          "the dataset produced a tensor with shape ",
          outputs[0].shape());
    }
    Variant& variant = outputs[0].scalar<Variant>()();
    CompressedElement* compressed = variant.get<CompressedElement>();
    if (compressed == nullptr) {
      return errors::FailedPrecondition(
          "Expected dataset to produce a CompressedElement variant tensor, but "
          "it produced ",
          variant.TypeName());
    }
    compressed->Swap(response->mutable_compressed_element());
  }
  response->set_end_of_sequence(end_of_sequence);

  return Status::OK();
}

Status DataServiceWorkerImpl::MakeDispatcherStub(
    std::unique_ptr<DispatcherService::Stub>* stub) {
  ::grpc::ChannelArguments args;
  std::shared_ptr<::grpc::ChannelCredentials> credentials;
  TF_RETURN_IF_ERROR(CredentialsFactory::CreateClientCredentials(
      config_.protocol(), &credentials));
  auto channel = ::grpc::CreateCustomChannel(config_.dispatcher_address(),
                                             credentials, args);
  *stub = DispatcherService::NewStub(channel);
  return Status::OK();
}

Status DataServiceWorkerImpl::Register(DispatcherService::Stub* dispatcher_stub)
    LOCKS_EXCLUDED(mu_) {
  VLOG(3) << "Registering with dispatcher at " << config_.dispatcher_address();
  RegisterWorkerRequest req;
  req.set_worker_address(worker_address_);
  RegisterWorkerResponse resp;
  grpc::ClientContext ctx;
  grpc::Status s = dispatcher_stub->RegisterWorker(&ctx, req, &resp);
  if (!s.ok()) {
    return grpc_util::WrapError("Failed to register worker", s);
  }
  for (const TaskDef& task : resp.tasks()) {
    mutex_lock l(mu_);
    TF_RETURN_IF_ERROR(ProcessTaskInternal(task));
  }
  VLOG(3) << "Registered worker with address " << worker_address_;
  return Status::OK();
}

void DataServiceWorkerImpl::BackgroundThread(
    DispatcherService::Stub* dispatcher_ptr) LOCKS_EXCLUDED(mu_) {
  std::unique_ptr<DispatcherService::Stub> dispatcher =
      absl::WrapUnique(dispatcher_ptr);
  while (true) {
    {
      mutex_lock l(mu_);
      while (!cancelled_ && pending_completed_tasks_.empty()) {
        background_cv_.wait(l);
      }
      if (cancelled_) {
        VLOG(3) << "Background thread shutting down";
        return;
      }
    }
    Status s = SendTaskUpdates(dispatcher.get());
    if (!s.ok()) {
      LOG(WARNING) << "Failed to send task updates to dispatcher: " << s;
      Env::Default()->SleepForMicroseconds(kRetryIntervalMicros);
    }
  }
}

Status DataServiceWorkerImpl::SendTaskUpdates(
    DispatcherService::Stub* dispatcher) LOCKS_EXCLUDED(mu_) {
  WorkerUpdateRequest req;
  {
    mutex_lock l(mu_);
    VLOG(3) << "Sending " << pending_completed_tasks_.size()
            << " task updates to dispatcher";
    req.set_worker_address(worker_address_);
    for (int task_id : pending_completed_tasks_) {
      TaskProgress* update = req.add_updates();
      update->set_task_id(task_id);
      update->set_completed(true);
    }
  }

  WorkerUpdateResponse resp;
  grpc::ClientContext ctx;
  grpc::Status s = dispatcher->WorkerUpdate(&ctx, req, &resp);
  if (!s.ok()) {
    return grpc_util::WrapError("Failed to send task updates", s);
  }
  mutex_lock l(mu_);
  for (const auto& update : req.updates()) {
    pending_completed_tasks_.erase(update.task_id());
  }
  VLOG(3) << "Sent " << req.updates().size() << " task updates ";
  return Status::OK();
}

}  // namespace data
}  // namespace tensorflow
