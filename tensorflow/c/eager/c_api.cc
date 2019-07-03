/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/c/eager/c_api.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// clang-format off
// Required for IS_MOBILE_PLATFORM
#include "tensorflow/core/platform/platform.h"
// clang-format on

#include "absl/memory/memory.h"
#include "tensorflow/c/c_api.h"
#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/c/eager/c_api_internal.h"
#include "tensorflow/core/common_runtime/eager/context.h"
#include "tensorflow/core/framework/device_attributes.pb.h"
#include "tensorflow/core/platform/host_info.h"
#include "tensorflow/core/platform/platform.h"  // NOLINT
#ifdef TENSORFLOW_EAGER_USE_XLA
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#endif  // TENSORFLOW_EAGER_USE_XLA
#include "tensorflow/core/common_runtime/copy_tensor.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/device_set.h"
#include "tensorflow/core/common_runtime/eager/attr_builder.h"
#include "tensorflow/core/common_runtime/eager/copy_to_device_node.h"
#include "tensorflow/core/common_runtime/eager/execute.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/rendezvous_mgr.h"
#if !defined(IS_MOBILE_PLATFORM)
#include "tensorflow/core/distributed_runtime/remote_device.h"
#include "tensorflow/core/distributed_runtime/rpc/eager/grpc_eager_client.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_channel.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_server_lib.h"
#include "tensorflow/core/distributed_runtime/rpc/rpc_rendezvous_mgr.h"
#include "tensorflow/core/distributed_runtime/server_lib.h"
#include "tensorflow/core/distributed_runtime/worker_env.h"
#endif  // !IS_MOBILE_PLATFORM
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/rendezvous.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/gtl/stl_util.h"
#include "tensorflow/core/lib/random/random.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/public/version.h"

using tensorflow::int64;
using tensorflow::string;

namespace {

const tensorflow::OpDef* GetOpDef(TFE_Op* op, TF_Status* status) {
  if (op->inference_ctx) {
    return op->inference_ctx->op_def;
  }
  const tensorflow::OpDef* op_def;
  status->status =
      tensorflow::OpDefForOp(op->operation.Name().c_str(), &op_def);
  return op_def;
}

bool IsCPU(const tensorflow::Device* d) {
  return d == nullptr || d->tensorflow_gpu_device_info() == nullptr;
}

string DeviceName(const tensorflow::Device* d) {
  return (d == nullptr) ? "cpu:0" : d->name();
}

#if !defined(IS_MOBILE_PLATFORM)
tensorflow::Status GetAllRemoteDevices(
    const std::vector<string>& remote_workers,
    tensorflow::WorkerCacheInterface* worker_cache,
    std::unique_ptr<tensorflow::DeviceMgr>* device_mgr) {
  std::vector<std::unique_ptr<tensorflow::Device>> remote_devices;
  tensorflow::Status status;
  // TODO(nareshmodi) do this in parallel instead of serially.
  for (const string& remote_worker : remote_workers) {
    tensorflow::Notification n;
    tensorflow::NewRemoteDevices(
        tensorflow::Env::Default(), worker_cache, remote_worker,
        [&status, &n, &remote_devices](
            const tensorflow::Status& s,
            std::vector<tensorflow::Device*>* devices) {
          status = s;
          if (s.ok()) {
            for (tensorflow::Device* d : *devices) {
              remote_devices.emplace_back(d);
            }
          }
          n.Notify();
        });
    n.WaitForNotification();
  }
  std::unique_ptr<tensorflow::DeviceMgr> remote_device_mgr(
      new tensorflow::DeviceMgr(std::move(remote_devices)));

  TF_RETURN_IF_ERROR(status);

  *device_mgr = std::move(remote_device_mgr);
  return tensorflow::Status::OK();
}

tensorflow::Status CreateRemoteContexts(
    const std::vector<string>& remote_workers, tensorflow::uint64 context_id,
    int keep_alive_secs, const tensorflow::ServerDef& server_def,
    tensorflow::eager::EagerClientCache* remote_eager_workers, bool async,
    const tensorflow::eager::CreateContextRequest& base_request) {
  for (int i = 0; i < remote_workers.size(); i++) {
    const string& remote_worker = remote_workers[i];

    tensorflow::eager::CreateContextRequest request(base_request);
    tensorflow::eager::CreateContextResponse response;
    request.set_context_id(context_id);
    tensorflow::DeviceNameUtils::ParsedName parsed_name;
    if (!tensorflow::DeviceNameUtils::ParseFullName(remote_worker,
                                                    &parsed_name)) {
      return tensorflow::errors::InvalidArgument(
          "Unable to parse ", remote_worker, " as a device name");
    }
    *request.mutable_server_def() = server_def;
    request.mutable_server_def()->set_job_name(parsed_name.job);
    request.mutable_server_def()->set_task_index(parsed_name.task);
    request.set_async(async);
    request.set_keep_alive_secs(keep_alive_secs);
    tensorflow::eager::EagerClient* eager_client;
    TF_RETURN_IF_ERROR(
        remote_eager_workers->GetClient(remote_worker, &eager_client));
    if (eager_client == nullptr) {
      return tensorflow::errors::Internal(
          "Cannot find a client for the given target:", remote_worker);
    }
    tensorflow::Notification n;
    tensorflow::Status status;
    // TODO(nareshmodi) do this in parallel instead of serially.
    eager_client->CreateContextAsync(
        &request, &response, [&status, &n](const tensorflow::Status& s) {
          status = s;
          n.Notify();
        });
    n.WaitForNotification();
    TF_RETURN_IF_ERROR(status);
  }
  return tensorflow::Status::OK();
}

tensorflow::Status UpdateTFE_ContextWithServerDef(
    int keep_alive_secs, const tensorflow::ServerDef& server_def,
    TFE_Context* ctx) {
  // We don't use the TF_RETURN_IF_ERROR macro directly since that destroys the
  // server object (which currently CHECK-fails) and we miss the error, instead,
  // we log the error, and then return to allow the user to see the error
  // message.
#define LOG_AND_RETURN_IF_ERROR(...)                    \
  do {                                                  \
    const ::tensorflow::Status _status = (__VA_ARGS__); \
    if (TF_PREDICT_FALSE(!_status.ok())) {              \
      LOG(ERROR) << _status.error_message();            \
      return _status;                                   \
    }                                                   \
  } while (0);

  string worker_name =
      tensorflow::strings::StrCat("/job:", server_def.job_name(),
                                  "/replica:0/task:", server_def.task_index());

  std::unique_ptr<tensorflow::ServerInterface> server;
  LOG_AND_RETURN_IF_ERROR(tensorflow::NewServer(server_def, &server));

  tensorflow::GrpcServer* grpc_server =
      dynamic_cast<tensorflow::GrpcServer*>(server.get());
  if (grpc_server == nullptr) {
    LOG_AND_RETURN_IF_ERROR(tensorflow::errors::Internal(
        "Currently, TFE_NewContext only supports tensorflow::GrpcServer."));
  }

  LOG_AND_RETURN_IF_ERROR(grpc_server->Start());

  tensorflow::uint64 context_id = tensorflow::random::New64();

  std::vector<string> remote_workers;
  grpc_server->master_env()->worker_cache->ListWorkers(&remote_workers);
  remote_workers.erase(
      std::remove(remote_workers.begin(), remote_workers.end(), worker_name),
      remote_workers.end());

  std::unique_ptr<tensorflow::DeviceMgr> remote_device_mgr;
  LOG_AND_RETURN_IF_ERROR(GetAllRemoteDevices(
      remote_workers, grpc_server->master_env()->worker_cache,
      &remote_device_mgr));

  std::vector<tensorflow::DeviceAttributes> cluster_device_attributes;
  remote_device_mgr->ListDeviceAttributes(&cluster_device_attributes);

  std::vector<tensorflow::DeviceAttributes> local_device_attributes;
  grpc_server->worker_env()->device_mgr->ListDeviceAttributes(
      &local_device_attributes);

  // This request make sure that we can create Rendevzous properly between
  // Local and Remote context.
  tensorflow::eager::CreateContextRequest base_request;
  for (const auto& da : cluster_device_attributes) {
    *base_request.add_cluster_device_attributes() = da;
  }
  for (const auto& da : local_device_attributes) {
    *base_request.add_cluster_device_attributes() = da;
  }

  std::unique_ptr<tensorflow::eager::EagerClientCache> remote_eager_workers;
  LOG_AND_RETURN_IF_ERROR(
      grpc_server->master_env()->worker_cache->GetEagerClientCache(
          &remote_eager_workers));

  // Initialize remote eager workers.
  LOG_AND_RETURN_IF_ERROR(CreateRemoteContexts(
      remote_workers, context_id, keep_alive_secs, server_def,
      remote_eager_workers.get(), ctx->context->Async(), base_request));

  tensorflow::RemoteRendezvous* r =
      grpc_server->worker_env()->rendezvous_mgr->Find(context_id);

  auto session_name = tensorflow::strings::StrCat("eager_", context_id);
  TF_RETURN_IF_ERROR(grpc_server->worker_env()->session_mgr->CreateSession(
      session_name, server_def, base_request.cluster_device_attributes(),
      true));

  std::shared_ptr<tensorflow::WorkerSession> worker_session;
  TF_RETURN_IF_ERROR(
      grpc_server->worker_env()->session_mgr->WorkerSessionForSession(
          session_name, &worker_session));

  // Initialize remote tensor communication based on worker session.
  TF_RETURN_IF_ERROR(r->Initialize(worker_session.get()));

  auto* device_mgr = grpc_server->worker_env()->device_mgr;

  return ctx->context->InitializeRemoteMaster(
      std::move(server), grpc_server->worker_env(), worker_session,
      std::move(remote_eager_workers), std::move(remote_device_mgr),
      remote_workers, context_id, r, device_mgr, keep_alive_secs,
      worker_session->cluster_flr.get());
#undef LOG_AND_RETURN_IF_ERROR
}
#endif  // !IS_MOBILE_PLATFORM

tensorflow::Status OpInferSingleInputAttrs(TFE_Op* op,
                                           TFE_TensorHandle* input) {
  TFE_OpInferenceContext* ictx = op->inference_ctx.get();
  const auto& input_def = ictx->op_def->input_arg(ictx->input_arg_idx++);
  if (!input_def.number_attr().empty() || !input_def.type_list_attr().empty()) {
    // Some clients that are still setting their input attributes manually are
    // adding input list to their op by calling `TFE_OpAddInput` for each of
    // its elements instead of calling `TFE_OpAddInputList`. When this happens,
    // we cannot detect the end of such list, thus lose track of the input
    // arguments in the op definition. To guarantee backward compatibility with
    // those clients, disable automatic inference in this case.
    op->inference_ctx.reset(nullptr);
    return tensorflow::Status::OK();
  }
  const std::string& type_attr = input_def.type_attr();
  if (!type_attr.empty() && ictx->attrs.find(type_attr) == ictx->attrs.end()) {
    op->operation.MutableAttrs()->Set(type_attr, input->handle->dtype);
    ictx->attrs.insert(type_attr);
  }
  return tensorflow::Status::OK();
}

void OpInferSingleTypeInputListAttrs(TFE_Op* op,
                                     const tensorflow::OpDef::ArgDef& input_def,
                                     TFE_TensorHandle** inputs,
                                     int num_inputs) {
  TFE_OpInferenceContext* ictx = op->inference_ctx.get();
  if (ictx->attrs.find(input_def.number_attr()) == ictx->attrs.end()) {
    op->operation.MutableAttrs()->Set(input_def.number_attr(), num_inputs);
    ictx->attrs.insert(input_def.number_attr());
  }
  if (ictx->attrs.find(input_def.type_attr()) == ictx->attrs.end()) {
    op->operation.MutableAttrs()->Set(input_def.type_attr(),
                                      inputs[0]->handle->dtype);
    ictx->attrs.insert(input_def.type_attr());
  }
}

void OpInferMixedTypeInputListAttrs(TFE_Op* op,
                                    const tensorflow::OpDef::ArgDef& input_def,
                                    TFE_TensorHandle** inputs, int num_inputs) {
  TFE_OpInferenceContext* ictx = op->inference_ctx.get();
  if (ictx->attrs.find(input_def.type_list_attr()) == ictx->attrs.end()) {
    std::unique_ptr<tensorflow::DataType[]> dtypes(
        new tensorflow::DataType[num_inputs]);
    for (int i = 0; i < num_inputs; ++i) {
      dtypes[i] = inputs[i]->handle->dtype;
    }
    op->operation.MutableAttrs()->Set(
        input_def.type_list_attr(),
        tensorflow::gtl::ArraySlice<const tensorflow::DataType>(dtypes.get(),
                                                                num_inputs));
    ictx->attrs.insert(input_def.type_list_attr());
  }
}

tensorflow::Status OpInferInputListAttrs(TFE_Op* op, TFE_TensorHandle** inputs,
                                         int num_inputs) {
  TFE_OpInferenceContext* ictx = op->inference_ctx.get();
  const auto& input_def = ictx->op_def->input_arg(ictx->input_arg_idx++);
  if (!input_def.type_list_attr().empty()) {
    OpInferMixedTypeInputListAttrs(op, input_def, inputs, num_inputs);
  } else if (!input_def.type_attr().empty() &&
             !input_def.number_attr().empty()) {
    OpInferSingleTypeInputListAttrs(op, input_def, inputs, num_inputs);
  } else {
    return tensorflow::errors::InvalidArgument("Invalid input list definition");
  }
  return tensorflow::Status::OK();
}

}  // namespace

extern "C" {

TFE_ContextOptions* TFE_NewContextOptions() { return new TFE_ContextOptions; }

void TFE_ContextOptionsSetConfig(TFE_ContextOptions* options, const void* proto,
                                 size_t proto_len, TF_Status* status) {
  TF_SetConfig(&options->session_options, proto, proto_len, status);
}

void TFE_ContextOptionsSetAsync(TFE_ContextOptions* options,
                                unsigned char enable) {
  options->async = enable;
}

void TFE_ContextOptionsSetDevicePlacementPolicy(
    TFE_ContextOptions* options, TFE_ContextDevicePlacementPolicy policy) {
  options->device_placement_policy = policy;
}

TF_CAPI_EXPORT extern void TFE_ContextSetAsyncForThread(TFE_Context* ctx,
                                                        unsigned char enable,
                                                        TF_Status* status) {
  status->status = ctx->context->SetAsyncForThread(enable);
}

void TFE_DeleteContextOptions(TFE_ContextOptions* options) { delete options; }

TFE_Context* TFE_NewContext(const TFE_ContextOptions* opts, TF_Status* status) {
  std::vector<std::unique_ptr<tensorflow::Device>> devices;
  status->status = tensorflow::DeviceFactory::AddDevices(
      opts->session_options.options, "/job:localhost/replica:0/task:0",
      &devices);
  if (!status->status.ok()) return nullptr;
  std::unique_ptr<tensorflow::DeviceMgr> device_mgr(
      new tensorflow::DeviceMgr(std::move(devices)));

  tensorflow::Rendezvous* r =
      new tensorflow::IntraProcessRendezvous(device_mgr.get());

  return new TFE_Context(opts->session_options.options,
                         opts->device_placement_policy, opts->mirroring_policy,
                         opts->async, device_mgr.release(),
                         /*device_mgr_owned*/ true, r,
                         tensorflow::GetDefaultCustomKernelCreator());
}

TFE_Context* TFE_NewContextFromSession(const TFE_ContextOptions* opts,
                                       TF_Session* sess, TF_Status* status) {
  const tensorflow::DeviceMgr* device_mgr = nullptr;
  status->status = sess->session->LocalDeviceManager(&device_mgr);
  if (!status->status.ok()) return nullptr;
  tensorflow::Rendezvous* r =
      new tensorflow::IntraProcessRendezvous(device_mgr);

  return new TFE_Context(opts->session_options.options,
                         opts->device_placement_policy, opts->mirroring_policy,
                         opts->async, device_mgr, /*device_mgr_owned*/ false, r,
                         tensorflow::GetDefaultCustomKernelCreator());
}

void TFE_DeleteContext(TFE_Context* ctx) { delete ctx; }

TF_DeviceList* TFE_ContextListDevices(TFE_Context* ctx, TF_Status* status) {
  TF_DeviceList* list = new TF_DeviceList;
  ctx->context->local_device_mgr()->ListDeviceAttributes(&list->response);
  if (ctx->context->remote_device_mgr()) {
    ctx->context->remote_device_mgr()->ListDeviceAttributes(&list->response);
  }
  return list;
}

void TFE_ContextClearCaches(TFE_Context* ctx) { ctx->context->ClearCaches(); }

// Set server_def on the context, possibly updating it.
TF_CAPI_EXPORT extern void TFE_ContextSetServerDef(TFE_Context* ctx,
                                                   int keep_alive_secs,
                                                   const void* proto,
                                                   size_t proto_len,
                                                   TF_Status* status) {
#if defined(IS_MOBILE_PLATFORM)
  status->status = tensorflow::errors::Unimplemented(
      "TFE_ContextSetServerDef not supported on mobile");
#else   // !defined(IS_MOBILE_PLATFORM)
  tensorflow::ServerDef server_def;
  if (!server_def.ParseFromArray(proto, proto_len)) {
    status->status = tensorflow::errors::InvalidArgument(
        "Invalid tensorflow.ServerDef protocol buffer");
    return;
  }
  status->status =
      UpdateTFE_ContextWithServerDef(keep_alive_secs, server_def, ctx);
#endif  // !IS_MOBILE_PLATFORM
}

void TFE_ContextSetThreadLocalDevicePlacementPolicy(
    TFE_Context* ctx, TFE_ContextDevicePlacementPolicy policy) {
  ctx->context->SetThreadLocalDevicePlacementPolicy(
      static_cast<tensorflow::ContextDevicePlacementPolicy>(policy));
}

// Note: this function looks up a thread local policy. So it should be called in
// the appropriate client thread. In particular, in async mode, it may not be
// safe to call this function from the async EagerExecutor threads.
extern TFE_ContextDevicePlacementPolicy TFE_ContextGetDevicePlacementPolicy(
    TFE_Context* ctx) {
  return static_cast<TFE_ContextDevicePlacementPolicy>(
      ctx->context->GetDevicePlacementPolicy());
}

void TFE_ContextAsyncWait(TFE_Context* ctx, TF_Status* status) {
  status->status = ctx->context->AsyncWait();
}

void TFE_ContextGetStatus(TFE_Context* ctx, TF_Status* status) {
  status->status = ctx->context->GetStatus();
}

void TFE_ContextAsyncClearError(TFE_Context* ctx) {
  ctx->context->ClearAsyncError();
}

TFE_TensorHandle* TFE_NewTensorHandle(TF_Tensor* t, TF_Status* status) {
  tensorflow::Tensor tensor;
  status->status = tensorflow::TF_TensorToTensor(t, &tensor);
  if (!status->status.ok()) return nullptr;
  return TFE_TensorHandle::CreateLocalHandle(tensor, status);
}

void TFE_DeleteTensorHandle(TFE_TensorHandle* h) {
  if (h == nullptr) return;
  VLOG(1) << "Deleting tensor handle " << h << " with internal handle "
          << h->handle;
  if (h->handle) {
    h->handle->Unref();
  }
  delete h;
}

TF_DataType TFE_TensorHandleDataType(TFE_TensorHandle* h) {
  return static_cast<TF_DataType>(h->handle->dtype);
}

int TFE_TensorHandleNumDims(TFE_TensorHandle* h, TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return -1;
  }
  int result;
  status->status = h->handle->NumDims(&result);
  return result;
}

int64_t TFE_TensorHandleNumElements(TFE_TensorHandle* h, TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return -1;
  }
  tensorflow::int64 result;
  status->status = h->handle->NumElements(&result);
  return result;
}

int64_t TFE_TensorHandleDim(TFE_TensorHandle* h, int dim_index,
                            TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return -1;
  }
  tensorflow::int64 result;
  status->status = h->handle->Dim(dim_index, &result);
  return result;
}

const char* TFE_TensorHandleDeviceName(TFE_TensorHandle* h, TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return nullptr;
  }
  tensorflow::Device* d = h->handle->op_device();
  return (d == nullptr) ? "/job:localhost/replica:0/task:0/device:CPU:0"
                        : d->name().c_str();
}

const char* TFE_TensorHandleBackingDeviceName(TFE_TensorHandle* h,
                                              TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return nullptr;
  }
  tensorflow::Device* d = h->handle->device();
  return (d == nullptr) ? "/job:localhost/replica:0/task:0/device:CPU:0"
                        : d->name().c_str();
}

TF_CAPI_EXPORT extern TFE_TensorHandle* TFE_TensorHandleCopySharingTensor(
    TFE_TensorHandle* h, TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return nullptr;
  }

  h->handle->Ref();

  return new TFE_TensorHandle(h->handle);
}

TF_Tensor* TFE_TensorHandleResolve(TFE_TensorHandle* h, TF_Status* status) {
  if (h == nullptr || h->handle == nullptr) {
    status->status = tensorflow::errors::InvalidArgument(
        "The passed in handle is a nullptr");
    return nullptr;
  }
  tensorflow::TensorHandle* handle = h->handle;

  // TODO(agarwal): move this implementation inside TFE_TensorHandle.
  if (handle->IsRemote()) {
    const tensorflow::Tensor* t = nullptr;
    tensorflow::TensorHandle* h_cpu = nullptr;
    status->status = EagerCopyToDevice(
        handle, handle->Context(), handle->Context()->HostCPU(), false, &h_cpu);
    if (!status->status.ok()) {
      return nullptr;
    }
    status->status = h_cpu->Tensor(&t);
    if (!status->status.ok()) {
      h_cpu->Unref();
      return nullptr;
    }
    TF_Tensor* retval = tensorflow::TF_TensorFromTensor(*t, status);
    h_cpu->Unref();
    return retval;
  } else {
    tensorflow::Tensor tensor;
    if (IsCPU(handle->device())) {
      const tensorflow::Tensor* src = nullptr;
      status->status = handle->Tensor(&src);
      if (!status->status.ok()) return nullptr;
      tensor = *src;
    } else {
      tensorflow::EagerContext* ctx = handle->Context();
      status->status = h->handle->CopyToDevice(ctx, ctx->HostCPU(), &tensor);
      if (!status->status.ok()) return nullptr;
    }
    return tensorflow::TF_TensorFromTensor(tensor, status);
  }
}

TFE_Op* TFE_NewOp(TFE_Context* ctx, const char* op_or_function_name,
                  TF_Status* status) {
  const char* name = op_or_function_name;  // Shorthand
  const tensorflow::AttrTypeMap* types;
  bool is_function = false;
  status->status = tensorflow::AttrTypeMapForOp(name, &types, &is_function);
  if (!status->status.ok()) {
    return nullptr;
  }
  if (!is_function) {
    const tensorflow::OpDef* op_def;
    status->status = tensorflow::OpDefForOp(op_or_function_name, &op_def);
    if (!status->status.ok()) {
      return nullptr;
    }
    return new TFE_Op(ctx, name, false, types,
                      new TFE_OpInferenceContext(op_def));
  }
  if (!ctx->context->FindFunctionByName(name)) {
    status->status = tensorflow::errors::NotFound(
        "'", name,
        "' is neither a type of a primitive operation nor a name "
        "of a function registered in binary running on ",
        tensorflow::port::Hostname(),
        ". Make sure the operation or function is "
        "registered in the binary running in this process.");
    return nullptr;
  }
  return new TFE_Op(ctx, name, true, types, nullptr);
}

void TFE_DeleteOp(TFE_Op* op) { delete op; }

void TFE_OpSetDevice(TFE_Op* op, const char* device_name, TF_Status* status) {
  status->status = op->operation.SetDeviceName(device_name);
}

const char* TFE_OpGetDevice(TFE_Op* op, TF_Status* status) {
  tensorflow::Device* device = (op->operation.Device() == nullptr)
                                   ? op->operation.EagerContext()->HostCPU()
                                   : op->operation.Device();
  return device->name().c_str();
}

void TFE_OpSetXLACompilation(TFE_Op* op, unsigned char enable) {
  op->operation.SetUseXla(enable);
#ifndef TENSORFLOW_EAGER_USE_XLA
  LOG(WARNING) << "This call is a no-op, as the TensorFlow library is not "
                  "built with XLA support.";
#endif  // TENSORFLOW_EAGER_USE_XLA
}

void TFE_OpAddInput(TFE_Op* op, TFE_TensorHandle* input, TF_Status* status) {
  op->operation.AddInput(input->handle);
  if (op->inference_ctx) {
    status->status = OpInferSingleInputAttrs(op, input);
  }
}

void TFE_OpAddInputList(TFE_Op* op, TFE_TensorHandle** inputs, int num_inputs,
                        TF_Status* status) {
  for (int i = 0; i < num_inputs; ++i) {
    op->operation.AddInput(inputs[i]->handle);
  }
  if (op->inference_ctx) {
    status->status = OpInferInputListAttrs(op, inputs, num_inputs);
  }
}

TF_AttrType TFE_OpGetAttrType(TFE_Op* op, const char* attr_name,
                              unsigned char* is_list, TF_Status* status) {
  TF_AttrType ret;
  status->status = tensorflow::AttrTypeByName(*op->operation.AttrTypes(),
                                              attr_name, &ret, is_list);
  return ret;
}

TF_AttrType TFE_OpNameGetAttrType(TFE_Context* ctx,
                                  const char* op_or_function_name,
                                  const char* attr_name, unsigned char* is_list,
                                  TF_Status* status) {
  TF_AttrType ret;
  TFE_Op* op = TFE_NewOp(ctx, op_or_function_name, status);
  if (!status->status.ok()) {
    return TF_ATTR_INT;  // Same dummy return as TFE_OpGetAttrType.
  }
  ret = TFE_OpGetAttrType(op, attr_name, is_list, status);
  TFE_DeleteOp(op);
  return ret;
}

void TFE_OpSetAttrString(TFE_Op* op, const char* attr_name, const void* value,
                         size_t length) {
  op->operation.MutableAttrs()->Set(
      attr_name,
      tensorflow::StringPiece(static_cast<const char*>(value), length));
}

void TFE_OpSetAttrInt(TFE_Op* op, const char* attr_name, int64_t value) {
  op->operation.MutableAttrs()->Set(attr_name, static_cast<int64>(value));
}

void TFE_OpSetAttrFloat(TFE_Op* op, const char* attr_name, float value) {
  op->operation.MutableAttrs()->Set(attr_name, value);
}

void TFE_OpSetAttrBool(TFE_Op* op, const char* attr_name, unsigned char value) {
  op->operation.MutableAttrs()->Set(attr_name, (value == 0) ? false : true);
}

void TFE_OpSetAttrType(TFE_Op* op, const char* attr_name, TF_DataType value) {
  op->operation.MutableAttrs()->Set(attr_name,
                                    static_cast<tensorflow::DataType>(value));
}

void TFE_OpSetAttrShape(TFE_Op* op, const char* attr_name, const int64_t* dims,
                        const int num_dims, TF_Status* out_status) {
  if (num_dims > tensorflow::TensorShape::MaxDimensions()) {
    TF_SetStatus(out_status, TF_INVALID_ARGUMENT,
                 tensorflow::strings::StrCat(
                     "Value specified for `", attr_name, "` has ", num_dims,
                     " dimensions which is over the limit of ",
                     tensorflow::TensorShape::MaxDimensions(), ".")
                     .c_str());
    return;
  }
  tensorflow::TensorShapeProto proto;
  if (num_dims < 0) {
    proto.set_unknown_rank(true);
  } else {
    for (int d = 0; d < num_dims; ++d) {
      proto.add_dim()->set_size(dims[d]);
    }
  }
  op->operation.MutableAttrs()->Set(attr_name, proto);
}

void TFE_OpSetAttrFunction(TFE_Op* op, const char* attr_name,
                           const TFE_Op* value) {
  tensorflow::AttrValue attr_value;
  tensorflow::NameAttrList* func = attr_value.mutable_func();
  func->set_name(value->operation.Name());
  value->operation.Attrs().FillAttrValueMap(func->mutable_attr());
  op->operation.MutableAttrs()->Set(attr_name, attr_value);
}

void TFE_OpSetAttrFunctionName(TFE_Op* op, const char* attr_name,
                               const char* data, size_t length) {
  tensorflow::AttrValue attr_value;
  tensorflow::NameAttrList* func = attr_value.mutable_func();
  func->set_name(data, length);
  op->operation.MutableAttrs()->Set(attr_name, attr_value);
}

void TFE_OpSetAttrTensor(TFE_Op* op, const char* attr_name, TF_Tensor* tensor,
                         TF_Status* status) {
  tensorflow::Tensor t;
  status->status = TF_TensorToTensor(tensor, &t);
  if (status->status.ok()) op->operation.MutableAttrs()->Set(attr_name, t);
}

void TFE_OpSetAttrStringList(TFE_Op* op, const char* attr_name,
                             const void* const* values, const size_t* lengths,
                             int num_values) {
  std::vector<tensorflow::StringPiece> v(num_values);
  for (int i = 0; i < num_values; ++i) {
    v[i] = tensorflow::StringPiece(static_cast<const char*>(values[i]),
                                   lengths[i]);
  }
  op->operation.MutableAttrs()->Set(attr_name, v);
}

void TFE_OpSetAttrFloatList(TFE_Op* op, const char* attr_name,
                            const float* values, int num_values) {
  op->operation.MutableAttrs()->Set(
      attr_name, tensorflow::gtl::ArraySlice<const float>(values, num_values));
}

void TFE_OpSetAttrIntList(TFE_Op* op, const char* attr_name,
                          const int64_t* values, int num_values) {
  op->operation.MutableAttrs()->Set(
      attr_name, tensorflow::gtl::ArraySlice<const int64>(
                     reinterpret_cast<const int64*>(values), num_values));
}

void TFE_OpSetAttrTypeList(TFE_Op* op, const char* attr_name,
                           const TF_DataType* values, int num_values) {
  op->operation.MutableAttrs()->Set(
      attr_name,
      tensorflow::gtl::ArraySlice<const tensorflow::DataType>(
          reinterpret_cast<const tensorflow::DataType*>(values), num_values));
}

void TFE_OpSetAttrBoolList(TFE_Op* op, const char* attr_name,
                           const unsigned char* values, int num_values) {
  std::unique_ptr<bool[]> b(new bool[num_values]);
  for (int i = 0; i < num_values; ++i) {
    b[i] = values[i];
  }
  op->operation.MutableAttrs()->Set(
      attr_name, tensorflow::gtl::ArraySlice<const bool>(b.get(), num_values));
}

void TFE_OpSetAttrShapeList(TFE_Op* op, const char* attr_name,
                            const int64_t** dims, const int* num_dims,
                            int num_values, TF_Status* out_status) {
  std::unique_ptr<tensorflow::TensorShapeProto[]> proto(
      new tensorflow::TensorShapeProto[num_values]);
  for (int i = 0; i < num_values; ++i) {
    const auto num_dims_i = num_dims[i];

    if (num_dims_i > tensorflow::TensorShape::MaxDimensions()) {
      TF_SetStatus(out_status, TF_INVALID_ARGUMENT,
                   tensorflow::strings::StrCat(
                       "Value specified for `", attr_name, "` has ", num_dims_i,
                       " dimensions which is over the limit of ",
                       tensorflow::TensorShape::MaxDimensions(), ".")
                       .c_str());
      return;
    }
    if (num_dims_i < 0) {
      proto[i].set_unknown_rank(true);
    } else {
      const int64_t* dims_i = dims[i];
      auto proto_i = &proto[i];
      for (int d = 0; d < num_dims_i; ++d) {
        proto_i->add_dim()->set_size(dims_i[d]);
      }
    }
  }
  op->operation.MutableAttrs()->Set(
      attr_name, tensorflow::gtl::ArraySlice<tensorflow::TensorShapeProto>(
                     proto.get(), num_values));
}

void TFE_OpSetAttrFunctionList(TFE_Op* op, const char* attr_name,
                               const TFE_Op** value, int num_values) {
  std::unique_ptr<tensorflow::NameAttrList[]> funcs(
      new tensorflow::NameAttrList[num_values]);
  for (int i = 0; i < num_values; i++) {
    funcs[i].set_name(value[i]->operation.Name());
    value[i]->operation.Attrs().FillAttrValueMap(funcs[i].mutable_attr());
  }
  op->operation.MutableAttrs()->Set(
      attr_name, tensorflow::gtl::ArraySlice<const tensorflow::NameAttrList>(
                     funcs.get(), num_values));
}

TF_CAPI_EXPORT extern int TFE_OpGetInputLength(TFE_Op* op,
                                               const char* input_name,
                                               TF_Status* status) {
  const tensorflow::OpDef* op_def = GetOpDef(op, status);
  if (!status->status.ok()) {
    return -1;
  }
  tensorflow::AttrValueMap attrs;
  op->operation.Attrs().FillAttrValueMap(&attrs);
  tensorflow::NameRangeMap name_ranges;
  status->status = tensorflow::NameRangesForNode(
      tensorflow::AttrSlice(&attrs), *op_def, &name_ranges, nullptr);
  if (!status->status.ok()) {
    return -1;
  }
  auto iter = name_ranges.find(input_name);
  if (iter == name_ranges.end()) {
    status->status = tensorflow::errors::InvalidArgument("Input '", input_name,
                                                         "' not found");
    return -1;
  }
  return iter->second.second - iter->second.first;
}

TF_CAPI_EXPORT extern int TFE_OpGetOutputLength(TFE_Op* op,
                                                const char* output_name,
                                                TF_Status* status) {
  const tensorflow::OpDef* op_def = GetOpDef(op, status);
  if (!status->status.ok()) {
    return -1;
  }
  tensorflow::AttrValueMap attrs;
  op->operation.Attrs().FillAttrValueMap(&attrs);
  tensorflow::NameRangeMap name_ranges;
  status->status = tensorflow::NameRangesForNode(
      tensorflow::AttrSlice(&attrs), *op_def, nullptr, &name_ranges);
  if (!status->status.ok()) {
    return -1;
  }
  auto iter = name_ranges.find(output_name);
  if (iter == name_ranges.end()) {
    status->status = tensorflow::errors::InvalidArgument(
        "Output '", output_name, "' not found");
    return -1;
  }
  return iter->second.second - iter->second.first;
}

void TFE_Execute(TFE_Op* op, TFE_TensorHandle** retvals, int* num_retvals,
                 TF_Status* status) {
  VLOG(1) << "Calling TFE_Execute() on op " << op;
  tensorflow::gtl::InlinedVector<tensorflow::TensorHandle*, 2> handle_retvals(
      *num_retvals);
  status->status =
      tensorflow::EagerExecute(&op->operation, &handle_retvals, num_retvals);
  if (!status->status.ok()) {
    return;
  }
  for (int i = 0; i < *num_retvals; ++i) {
    retvals[i] = new TFE_TensorHandle(handle_retvals[i]);
  }
}

TFE_TensorHandle* TFE_TensorHandleCopyToDevice(TFE_TensorHandle* h,
                                               TFE_Context* ctx,
                                               const char* device_name,
                                               TF_Status* status) {
  tensorflow::TensorHandle* handle = nullptr;
  tensorflow::Device* device;
  status->status = ctx->context->FindDeviceFromName(device_name, &device);
  if (!status->status.ok()) {
    return nullptr;
  }
  status->status = tensorflow::EagerCopyToDevice(h->handle, ctx->context,
                                                 device, false, &handle);
  if (status->status.ok()) {
    return new TFE_TensorHandle(handle);
  }
  return nullptr;
}

void TFE_ContextAddFunctionDef(TFE_Context* ctx,
                               const char* serialized_function_def, size_t size,
                               TF_Status* status) {
  tensorflow::FunctionDef function_def;
  if (!function_def.ParseFromArray(serialized_function_def, size)) {
    status->status =
        tensorflow::errors::InvalidArgument("Invalid FunctionDef proto");
    return;
  }
  status->status = ctx->context->AddFunctionDef(function_def);
}

void TFE_ContextAddFunction(TFE_Context* ctx, TF_Function* function,
                            TF_Status* status) {
  status->status = ctx->context->AddFunctionDef(function->fdef);
}

void TFE_ContextRemoveFunction(TFE_Context* ctx, const char* name,
                               TF_Status* status) {
  status->status = ctx->context->RemoveFunction(name);
}

unsigned char TFE_ContextHasFunction(TFE_Context* ctx, const char* name) {
  return ctx->context->FindFunctionDef(name) != nullptr;
}

void TFE_ContextEnableRunMetadata(TFE_Context* ctx) {
  ctx->context->SetShouldStoreGraphs(true);
  ctx->context->SetShouldStoreStepStats(true);
}

void TFE_ContextDisableRunMetadata(TFE_Context* ctx) {
  ctx->context->SetShouldStoreGraphs(false);
  ctx->context->SetShouldStoreStepStats(false);
}

}  // extern "C"

TFE_TensorHandle* TFE_NewTensorHandle(const tensorflow::Tensor& t,
                                      TF_Status* status) {
  return TFE_TensorHandle::CreateLocalHandle(t, status);
}

const tensorflow::Tensor* TFE_TensorHandleUnderlyingTensorInHostMemory(
    TFE_TensorHandle* h, TF_Status* status) {
  if (!h->handle->OnHostCPU()) {
    status->status = tensorflow::errors::FailedPrecondition(
        "TFE_TensorHandle is placed in device (not host) memory. Cannot return "
        "a tensorflow::Tensor");
    return nullptr;
  }

  const tensorflow::Tensor* t = nullptr;
  status->status = h->handle->Tensor(&t);
  if (!status->status.ok()) return nullptr;

  return t;
}

TFE_TensorHandle* TFE_TensorHandleMaybeCopyToHostCPU(TFE_TensorHandle* h,
                                                     TF_Status* status) {
  // TensorHandles created by PyFuncOp lack context and therefore could
  // not be copied.
  tensorflow::EagerContext* ctx = h->handle->Context();
  if (!h->handle->OnHostCPU() && ctx != nullptr) {
    tensorflow::TensorHandle* handle = nullptr;
    status->status = tensorflow::EagerCopyToDevice(
        h->handle, ctx, ctx->HostCPU(), false, &handle);
    if (status->status.ok()) {
      return new TFE_TensorHandle(handle);
    } else {
      return nullptr;
    }
  }
  return h;
}

void TFE_ContextExportRunMetadata(TFE_Context* ctx, TF_Buffer* buf,
                                  TF_Status* status) {
  TFE_ContextAsyncWait(ctx, status);
  if (!status->status.ok()) return;
  tensorflow::mutex_lock ml(*ctx->context->MetadataMu());
  status->status = MessageToBuffer(*ctx->context->RunMetadataProto(), buf);
  ctx->context->ClearRunMetadata();
}

namespace {
TFE_Op* GetFunc(TFE_Context* ctx, const tensorflow::NameAttrList& func,
                TF_Status* status) {
  TFE_Op* func_op = TFE_NewOp(ctx, func.name().data(), status);
  for (const auto& attr : func.attr()) {
    if (TF_GetCode(status) != TF_OK) return nullptr;
    SetOpAttrValueScalar(ctx, func_op, attr.second, attr.first.data(), status);
    if (TF_GetCode(status) != TF_OK) return nullptr;
  }
  return func_op;
}
}  // namespace

void TFE_ContextStartStep(TFE_Context* ctx) { ctx->context->StartStep(); }

void TFE_ContextEndStep(TFE_Context* ctx) { ctx->context->EndStep(); }

namespace tensorflow {
void SetOpAttrValueScalar(TFE_Context* ctx, TFE_Op* op,
                          const tensorflow::AttrValue& default_value,
                          const char* attr_name, TF_Status* status) {
  switch (default_value.value_case()) {
    case tensorflow::AttrValue::kS: {
      const string& v = default_value.s();
      TFE_OpSetAttrString(op, attr_name, v.data(), v.size());
      break;
    }
    case tensorflow::AttrValue::kI:
      TFE_OpSetAttrInt(op, attr_name, static_cast<int64_t>(default_value.i()));
      break;
    case tensorflow::AttrValue::kF:
      TFE_OpSetAttrFloat(op, attr_name, default_value.f());
      break;
    case tensorflow::AttrValue::kB:
      TFE_OpSetAttrBool(op, attr_name, default_value.b());
      break;
    case tensorflow::AttrValue::kType:
      TFE_OpSetAttrType(op, attr_name,
                        static_cast<TF_DataType>(default_value.type()));
      break;
    case tensorflow::AttrValue::kShape: {
      const auto& tensor_shape = default_value.shape();
      if (tensor_shape.unknown_rank()) {
        TFE_OpSetAttrShape(op, attr_name, nullptr, -1, status);
      } else {
        const auto num_dims = tensor_shape.dim_size();
        std::unique_ptr<int64_t[]> dims(new int64_t[num_dims]);
        for (int i = 0; i < num_dims; ++i) {
          dims[i] = tensor_shape.dim(i).size();
        }
        TFE_OpSetAttrShape(op, attr_name, dims.get(), num_dims, status);
      }
    } break;
    case tensorflow::AttrValue::kFunc: {
      const auto func_op = GetFunc(ctx, default_value.func(), status);
      if (TF_GetCode(status) != TF_OK) return;
      // TODO(nareshmodi): TFE_OpSetAttrFunction and TFE_OpSetAttrFunctionList
      // require TFE_Op* and just convert it internally a NameAttrValue, so
      // consider adding an overload to the C API to make this case easier.
      TFE_OpSetAttrFunction(op, attr_name, func_op);
    } break;
    case tensorflow::AttrValue::kList:
      TF_FALLTHROUGH_INTENDED;
    case tensorflow::AttrValue::kTensor:
      TF_FALLTHROUGH_INTENDED;
    case tensorflow::AttrValue::kPlaceholder:
      TF_FALLTHROUGH_INTENDED;
    case tensorflow::AttrValue::VALUE_NOT_SET:
      TF_SetStatus(
          status, TF_UNIMPLEMENTED,
          tensorflow::strings::StrCat("Unable to get setfor default value: ",
                                      default_value.DebugString())
              .data());
  }
}
}  // namespace tensorflow
