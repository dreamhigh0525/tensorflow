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

#include "tensorflow/core/data/service/dispatcher_impl.h"

#include <memory>
#include <tuple>
#include <utility>

#include "grpcpp/create_channel.h"
#include "grpcpp/impl/codegen/server_context.h"
#include "grpcpp/security/credentials.h"
#include "absl/memory/memory.h"
#include "tensorflow/core/data/service/common.pb.h"
#include "tensorflow/core/data/service/credentials_factory.h"
#include "tensorflow/core/data/service/data_service.h"
#include "tensorflow/core/data/service/dispatcher.pb.h"
#include "tensorflow/core/data/service/grpc_util.h"
#include "tensorflow/core/data/service/journal.h"
#include "tensorflow/core/data/service/worker.grpc.pb.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/kernels/data/dataset_utils.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/protobuf/data/experimental/service_config.pb.h"
#include "tensorflow/core/public/session_options.h"

namespace tensorflow {
namespace data {

namespace {
// The name of the journal directory inside the dispatcher's working directory.
constexpr char kJournalDir[] = "journal";

using Dataset = DispatcherState::Dataset;
using NamedJobKey = DispatcherState::NamedJobKey;
using Job = DispatcherState::Job;
using Task = DispatcherState::Task;

std::string JournalDir(const std::string& work_dir) {
  return io::JoinPath(work_dir, kJournalDir);
}

Status CreateWorkerStub(const std::string& address, const std::string& protocol,
                        std::unique_ptr<WorkerService::Stub>* stub) {
  ::grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(-1);
  std::shared_ptr<::grpc::ChannelCredentials> credentials;
  TF_RETURN_IF_ERROR(
      CredentialsFactory::CreateClientCredentials(protocol, &credentials));
  auto channel = ::grpc::CreateCustomChannel(address, credentials, args);
  *stub = WorkerService::NewStub(channel);
  return Status::OK();
}
}  // namespace

DataServiceDispatcherImpl::DataServiceDispatcherImpl(
    const experimental::DispatcherConfig& config)
    : config_(config) {
  if (!config_.work_dir().empty()) {
    journal_writer_ = absl::make_unique<FileJournalWriter>(
        Env::Default(), JournalDir(config_.work_dir()));
  }
}

Status DataServiceDispatcherImpl::Start() {
  if (config_.work_dir().empty()) {
    return Status::OK();
  }
  mutex_lock l(mu_);
  Update update;
  bool end_of_journal = false;
  FileJournalReader reader(Env::Default(), JournalDir(config_.work_dir()));
  Status s = reader.Read(&update, &end_of_journal);
  if (errors::IsNotFound(s)) {
    LOG(INFO) << "No journal found. Starting dispatcher from new state.";
    return Status::OK();
  }
  TF_RETURN_IF_ERROR(s);
  LOG(INFO) << "Restoring dispatcher state from journal in "
            << JournalDir(config_.work_dir());
  while (!end_of_journal) {
    TF_RETURN_IF_ERROR(ApplyWithoutJournaling(update));
    TF_RETURN_IF_ERROR(reader.Read(&update, &end_of_journal));
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::RegisterWorker(
    const RegisterWorkerRequest* request, RegisterWorkerResponse* response) {
  VLOG(3) << "Received register worker request";
  mutex_lock l(mu_);
  std::string worker_address = request->worker_address();
  if (!workers_.contains(worker_address)) {
    workers_[worker_address] =
        std::make_shared<Worker>(next_worker_id_++, worker_address);
  }
  int64 worker_id = workers_[worker_address]->worker_id;
  response->set_worker_id(worker_id);
  std::vector<std::shared_ptr<const Job>> jobs = state_.ListJobs();
  // Allocate tasks to the worker.
  for (const auto& job : jobs) {
    if (job->finished) {
      continue;
    }
    std::shared_ptr<const Task> task;
    TF_RETURN_IF_ERROR(CreateTask(job, worker_address, &task));
    TaskDef* task_def = response->add_tasks();
    std::shared_ptr<const Dataset> dataset;
    TF_RETURN_IF_ERROR(state_.DatasetFromId(job->dataset_id, &dataset));
    *(task_def->mutable_dataset()) = dataset->dataset_def;
    task_def->set_dataset_id(job->dataset_id);
    task_def->set_job_id(job->job_id);
    task_def->set_task_id(task->task_id);
  }

  VLOG(1) << "Registered worker at address " << request->worker_address()
          << " with id " << worker_id;
  return Status::OK();
}

Status DataServiceDispatcherImpl::WorkerUpdate(
    const WorkerUpdateRequest* request, WorkerUpdateResponse* response) {
  mutex_lock l(mu_);
  for (auto& update : request->updates()) {
    int64 task_id = update.task_id();
    std::shared_ptr<const Task> task;
    TF_RETURN_IF_ERROR(state_.TaskFromId(task_id, &task));
    if (update.completed()) {
      if (task->finished) {
        VLOG(1) << "Received completion update for already-finished task "
                << task->task_id << " on worker " << task->worker_address;
        continue;
      }
      Update update;
      FinishTaskUpdate* finish_task = update.mutable_finish_task();
      finish_task->set_task_id(task_id);
      TF_RETURN_IF_ERROR(Apply(update));
      VLOG(3) << "Task " << task_id << " from job " << task->job_id
              << " completed";
    }
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::GetOrRegisterDataset(
    const GetOrRegisterDatasetRequest* request,
    GetOrRegisterDatasetResponse* response) {
  uint64 fingerprint;
  TF_RETURN_IF_ERROR(HashGraph(request->dataset().graph(), &fingerprint));
  mutex_lock l(mu_);
  VLOG(4) << "Registering dataset graph: "
          << request->dataset().graph().DebugString();
  std::shared_ptr<const Dataset> dataset;
  Status s = state_.DatasetFromFingerprint(fingerprint, &dataset);
  if (s.ok()) {
    int64 id = dataset->dataset_id;
    VLOG(3) << "Received duplicate RegisterDataset request with fingerprint "
            << fingerprint << ". Returning id " << id;
    response->set_dataset_id(id);
    return Status::OK();
  } else if (!errors::IsNotFound(s)) {
    return s;
  }

  int64 id;
  TF_RETURN_IF_ERROR(RegisterDataset(fingerprint, request->dataset(), &id));
  response->set_dataset_id(id);
  VLOG(3) << "Registered new dataset with id " << id;
  return Status::OK();
}

Status DataServiceDispatcherImpl::RegisterDataset(uint64 fingerprint,
                                                  const DatasetDef& dataset,
                                                  int64* dataset_id)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  *dataset_id = state_.NextAvailableDatasetId();
  Update update;
  RegisterDatasetUpdate* register_dataset = update.mutable_register_dataset();
  register_dataset->set_dataset_id(*dataset_id);
  register_dataset->set_fingerprint(fingerprint);
  *register_dataset->mutable_dataset_def() = dataset;
  return Apply(update);
}

Status DataServiceDispatcherImpl::CreateJob(const CreateJobRequest* request,
                                            CreateJobResponse* response) {
  VLOG(3) << "Received create job request for dataset id "
          << request->dataset_id();
  ProcessingMode processing_mode = ProcessingMode(request->processing_mode());
  std::shared_ptr<const Job> job;
  std::vector<std::shared_ptr<const Task>> tasks;
  {
    mutex_lock l(mu_);
    TF_RETURN_IF_ERROR(CreateJob(request->dataset_id(), processing_mode,
                                 absl::optional<NamedJobKey>(), &job));
    TF_RETURN_IF_ERROR(CreateTasksForJob(job, &tasks));
  }
  response->set_job_id(job->job_id);
  TF_RETURN_IF_ERROR(AssignTasks(tasks));

  VLOG(3) << "Creating job " << job->job_id << " for dataset "
          << request->dataset_id();
  return Status::OK();
}

Status DataServiceDispatcherImpl::GetOrCreateJob(
    const GetOrCreateJobRequest* request, GetOrCreateJobResponse* response) {
  VLOG(3) << "Received get or create job request for dataset id "
          << request->dataset_id() << " with name " << request->job_name()
          << " and index " << request->job_name_index();
  NamedJobKey key(request->job_name(), request->job_name_index());
  ProcessingMode requested_processing_mode =
      ProcessingMode(request->processing_mode());
  std::shared_ptr<const Job> job;
  std::vector<std::shared_ptr<const Task>> tasks;
  {
    mutex_lock l(mu_);
    Status s = state_.NamedJobByKey(key, &job);
    if (s.ok()) {
      TF_RETURN_IF_ERROR(ValidateMatchingJob(job, requested_processing_mode,
                                             request->dataset_id()));
      response->set_job_id(job->job_id);
      VLOG(3) << "Found existing job for name=" << key.name
              << ", index=" << key.index << ". job_id: " << job->job_id;
      return Status::OK();
    } else if (!errors::IsNotFound(s)) {
      return s;
    }
    TF_RETURN_IF_ERROR(
        CreateJob(request->dataset_id(), requested_processing_mode, key, &job));
    TF_RETURN_IF_ERROR(CreateTasksForJob(job, &tasks));
  }
  TF_RETURN_IF_ERROR(AssignTasks(tasks));
  response->set_job_id(job->job_id);
  VLOG(3) << "Created job " << job->job_id << " for dataset "
          << request->dataset_id() << " and name " << request->job_name();
  return Status::OK();
}

// Validates that the job matches the given processing_mode and dataset_id.
Status DataServiceDispatcherImpl::ValidateMatchingJob(
    std::shared_ptr<const Job> job, ProcessingMode processing_mode,
    int64 dataset_id) EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  DCHECK(job->named_job_key.has_value());
  std::string job_name = job->named_job_key->name;
  if (job->processing_mode != processing_mode) {
    std::string requested = ProcessingModeToString(processing_mode);
    std::string actual = ProcessingModeToString(job->processing_mode);
    return errors::FailedPrecondition(
        "Found a job with name ", job_name, ", but the processing mode <",
        actual, "> doesn't match the requested processing mode <", requested,
        ">.");
  }
  if (job->dataset_id != dataset_id) {
    return errors::FailedPrecondition(
        "Found a job with name ", job_name, ", but the dataset id <",
        job->dataset_id, "> doesn't match the requested dataset id <",
        dataset_id, ">.");
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::CreateJob(
    int64 dataset_id, ProcessingMode processing_mode,
    absl::optional<NamedJobKey> named_job_key, std::shared_ptr<const Job>* job)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  switch (processing_mode) {
    case ProcessingMode::PARALLEL_EPOCHS:
      break;
    case ProcessingMode::ONE_EPOCH:
      return errors::Unimplemented(
          "CreateJob only supports the PARALLEL_EPOCHS job mode. "
          "ONE_EPOCH is not currently supported.");
    default:
      return errors::Unimplemented("ProcessingMode ",
                                   ProcessingModeToString(processing_mode),
                                   " not recognized");
  }
  int64 job_id = state_.NextAvailableJobId();
  Update update;
  CreateJobUpdate* create_job = update.mutable_create_job();
  create_job->set_job_id(job_id);
  create_job->set_dataset_id(dataset_id);
  create_job->set_processing_mode(ProcessingModeDef(processing_mode));
  if (named_job_key.has_value()) {
    NamedJobKeyDef* key = create_job->mutable_named_job_key();
    key->set_name(named_job_key->name);
    key->set_index(named_job_key->index);
  }
  TF_RETURN_IF_ERROR(Apply(update));
  TF_RETURN_IF_ERROR(state_.JobFromId(job_id, job));
  return Status::OK();
}

Status DataServiceDispatcherImpl::CreateTasksForJob(
    std::shared_ptr<const Job> job,
    std::vector<std::shared_ptr<const Task>>* tasks)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  tasks->clear();
  tasks->reserve(workers_.size());
  for (const auto& it : workers_) {
    std::shared_ptr<Worker> worker = it.second;
    std::shared_ptr<const Task> task;
    TF_RETURN_IF_ERROR(CreateTask(job, worker->address, &task));
    tasks->push_back(task);
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::CreateTask(std::shared_ptr<const Job> job,
                                             const std::string& worker_address,
                                             std::shared_ptr<const Task>* task)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  int64 task_id = state_.NextAvailableTaskId();
  Update update;
  CreateTaskUpdate* create_task = update.mutable_create_task();
  create_task->set_task_id(task_id);
  create_task->set_job_id(job->job_id);
  create_task->set_dataset_id(job->dataset_id);
  create_task->set_worker_address(worker_address);
  TF_RETURN_IF_ERROR(Apply(update));
  TF_RETURN_IF_ERROR(state_.TaskFromId(task_id, task));
  return Status::OK();
}

Status DataServiceDispatcherImpl::AssignTasks(
    std::vector<std::shared_ptr<const Task>> tasks) LOCKS_EXCLUDED(mu_) {
  for (const auto& task : tasks) {
    TF_RETURN_IF_ERROR(AssignTask(task));
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::EnsureWorkerStubInitialized(Worker* worker) {
  if (!worker->stub) {
    TF_RETURN_IF_ERROR(
        CreateWorkerStub(worker->address, config_.protocol(), &worker->stub));
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::AssignTask(std::shared_ptr<const Task> task)
    LOCKS_EXCLUDED(mu_) {
  grpc::ClientContext client_ctx;
  ProcessTaskRequest req;
  TaskDef* task_def = req.mutable_task();
  task_def->set_dataset_id(task->dataset_id);
  std::shared_ptr<Worker> worker;
  {
    mutex_lock l(mu_);
    worker = workers_[task->worker_address];
    std::shared_ptr<const Dataset> dataset;
    TF_RETURN_IF_ERROR(state_.DatasetFromId(task->dataset_id, &dataset));
    *task_def->mutable_dataset() = dataset->dataset_def;
  }
  if (!worker) {
    return errors::NotFound("No worker found for address ",
                            task->worker_address);
  }
  task_def->set_task_id(task->task_id);
  ProcessTaskResponse resp;
  TF_RETURN_IF_ERROR(EnsureWorkerStubInitialized(worker.get()));
  grpc::Status s = worker->stub->ProcessTask(&client_ctx, req, &resp);
  if (!s.ok()) {
    return grpc_util::WrapError(
        absl::StrCat("Failed to submit task to worker ", worker->address), s);
  }
  return Status::OK();
}

Status DataServiceDispatcherImpl::GetTasks(const GetTasksRequest* request,
                                           GetTasksResponse* response) {
  mutex_lock l(mu_);
  VLOG(3) << "Looking up tasks for job id " << request->job_id();
  std::vector<std::shared_ptr<const Task>> tasks;
  TF_RETURN_IF_ERROR(state_.TasksForJob(request->job_id(), &tasks));
  for (const auto& task : tasks) {
    TaskInfo* task_info = response->mutable_task_info()->Add();
    task_info->set_worker_address(task->worker_address);
    task_info->set_id(task->task_id);
  }
  std::shared_ptr<const Job> job;
  TF_RETURN_IF_ERROR(state_.JobFromId(request->job_id(), &job));
  response->set_job_finished(job->finished);
  VLOG(3) << "Found " << response->task_info_size() << " tasks for job id "
          << request->job_id();
  return Status::OK();
}

Status DataServiceDispatcherImpl::GetWorkers(const GetWorkersRequest* request,
                                             GetWorkersResponse* response) {
  mutex_lock l(mu_);
  VLOG(3) << "Enter GetWorkers";
  for (const auto& it : workers_) {
    std::shared_ptr<Worker> worker = it.second;
    WorkerInfo* info = response->add_workers();
    info->set_address(worker->address);
    info->set_id(worker->worker_id);
  }
  VLOG(3) << "Returning list of " << workers_.size()
          << " workers from GetWorkers";
  return Status::OK();
}

Status DataServiceDispatcherImpl::ApplyWithoutJournaling(const Update& update)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  return state_.Apply(update);
}

Status DataServiceDispatcherImpl::Apply(const Update& update)
    EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (journal_writer_.has_value()) {
    TF_RETURN_IF_ERROR(journal_writer_.value()->Write(update));
  }
  return state_.Apply(update);
}

}  // namespace data
}  // namespace tensorflow
