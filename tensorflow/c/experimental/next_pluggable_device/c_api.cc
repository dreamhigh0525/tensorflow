/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/c/experimental/next_pluggable_device/c_api.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/c/kernels_experimental.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/c/tf_status_internal.h"
#include "tensorflow/c/tf_tensor.h"
#include "tensorflow/c/tf_tensor_internal.h"
#include "tensorflow/compiler/jit/xla_launch_util.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_c_api_client.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"
#include "tensorflow/core/common_runtime/next_pluggable_device/next_pluggable_device.h"
#include "tensorflow/core/common_runtime/next_pluggable_device/plugin_resource.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/tfrt/common/async_value_tensor.h"
#include "tensorflow/core/tfrt/common/pjrt_util.h"
#include "tensorflow/tsl/distributed_runtime/coordination/coordination_service_agent.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/statusor.h"

TF_Device* TF_GetDevice(TF_OpKernelContext* ctx) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelContext*>(ctx);
  return reinterpret_cast<TF_Device*>(cc_ctx->device());
}

size_t TF_GetDeviceOrdinal(TF_Device* device) {
  // TODO(chuanhao): make GetDeviceOrdinal a virtual member function in the base
  // device class, instead of casting to `NextPluggableDevice`.
  auto cc_device = reinterpret_cast<tensorflow::NextPluggableDevice*>(device);
  return cc_device->GetDeviceOrdinal();
}

// --------------------------  Resource  ---------------------------------------
void TF_CreatePluginResource(TF_OpKernelContext* ctx,
                             const char* container_name,
                             const char* plugin_resource_name,
                             void* plugin_resource, void (*delete_func)(void*),
                             TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelContext*>(ctx);
  tensorflow::PluginResource* cc_resource_ptr = new tensorflow::PluginResource(
      plugin_resource, plugin_resource_name, delete_func);
  auto cc_status =
      cc_ctx->resource_manager()->Create<tensorflow::PluginResource>(
          container_name, plugin_resource_name, cc_resource_ptr);
  Set_TF_Status_from_Status(status, cc_status);
}

void TF_LookupOrCreatePluginResource(
    TF_OpKernelContext* ctx, const char* container_name,
    const char* plugin_resource_name, void** result_plugin_resource,
    void* (*create_func)(void*), void* create_func_args,
    void (*delete_func)(void*), TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelContext*>(ctx);
  auto* resource_mgr = cc_ctx->resource_manager();
  tensorflow::core::RefCountPtr<tensorflow::PluginResource>
      tf_plugin_resource_ptr;
  tensorflow::PluginResource* tf_plugin_resource = nullptr;

  auto cc_status = resource_mgr->LookupOrCreate<tensorflow::PluginResource>(
      container_name, plugin_resource_name, &tf_plugin_resource,
      [plugin_resource_name, create_func, create_func_args,
       delete_func](tensorflow::PluginResource** new_resource) {
        void* opaque_plugin_resource = create_func(create_func_args);
        *new_resource = new tensorflow::PluginResource(
            opaque_plugin_resource, plugin_resource_name, delete_func);
        return tensorflow::OkStatus();
      });

  if (cc_status.ok()) {
    tf_plugin_resource_ptr.reset(tf_plugin_resource);
    *result_plugin_resource = tf_plugin_resource_ptr->GetOpaquePluginResource();
  } else {
    *result_plugin_resource = nullptr;
  }
  Set_TF_Status_from_Status(status, cc_status);
}

// -------------------------  VariableInfo  ------------------------------------
struct TF_VariableInfo {
  TF_VariableInfo() = delete;
  // TF_VariableInfo is constructed here by TensorFlow, and will be passed to
  // plugin as a opaque pointer. Plugin will need to call C APIs below to
  // operate on TF_VaribleInfo (such as allocate temp tensor for the `var` held
  // by the underlying tensorflow::VariableInfo.
  TF_VariableInfo(int index, const std::string& name, tensorflow::Var* var) {
    var_info = tensorflow::VariableInfo{index, name, var};
  }

  tensorflow::VariableInfo var_info{0, "", nullptr};
};

TF_VariableInfo* TF_CreateVariableInfoFromContext(TF_OpKernelContext* ctx,
                                                  int index,
                                                  TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelContext*>(ctx);
  const tensorflow::Tensor& arg_tensor = cc_ctx->input(index);
  tsl::Status cc_status;
  if (arg_tensor.dtype() != tensorflow::DT_RESOURCE) {
    cc_status = tsl::errors::InvalidArgument(
        "Trying to obtain resource handle from Input[", index,
        "], which is not type DT_RESOURCE.");
    Set_TF_Status_from_Status(status, cc_status);
    return nullptr;
  }
  const tensorflow::ResourceHandle& handle =
      arg_tensor.flat<tensorflow::ResourceHandle>()(0);
  tensorflow::Var* variable;
  cc_status = tensorflow::LookupResource(cc_ctx, handle, &variable);
  return new TF_VariableInfo(index, handle.name(), variable);
}

void TF_LockVariableInfos(TF_VariableInfo** vars, int num_vars,
                          TF_Status* status) {
  std::vector<tensorflow::VariableInfo*> variable_ptrs;
  variable_ptrs.reserve(num_vars);
  for (int i = 0; i < num_vars; ++i) {
    variable_ptrs.push_back(&(vars[i]->var_info));
  }
  tsl::Status cc_status = LockVariables(absl::MakeSpan(variable_ptrs));
  tsl::Set_TF_Status_from_Status(status, cc_status);
}

void TF_AllocateTempForVariableInfo(TF_OpKernelContext* ctx,
                                    TF_VariableInfo* var_info,
                                    TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelContext*>(ctx);
  tsl::Status cc_status;
  if (var_info == nullptr) {
    cc_status = tsl::errors::InvalidArgument("TF_VariableInfo is NULL.");
    Set_TF_Status_from_Status(status, cc_status);
    return;
  }
  if (var_info->var_info.var() == nullptr) {
    cc_status = tsl::errors::InvalidArgument(
        "VariableInfo does not track a resource variable.");
    Set_TF_Status_from_Status(status, cc_status);
    return;
  }

  cc_status = cc_ctx->allocate_temp(var_info->var_info.var()->tensor()->dtype(),
                                    var_info->var_info.var()->tensor()->shape(),
                                    var_info->var_info.var()->tensor());
  Set_TF_Status_from_Status(status, cc_status);
}

TF_Tensor* TF_GetTensorFromVariableInfo(TF_VariableInfo* var_info,
                                        TF_Status* status) {
  tsl::Status cc_status;
  if (var_info == nullptr) {
    cc_status = tsl::errors::InvalidArgument("TF_VariableInfo is NULL.");
    Set_TF_Status_from_Status(status, cc_status);
    return nullptr;
  }
  if (var_info->var_info.var() == nullptr) {
    cc_status = tsl::errors::InvalidArgument(
        "VariableInfo does not track a resource variable.");
    Set_TF_Status_from_Status(status, cc_status);
    return nullptr;
  }

  tensorflow::Tensor* tensor = var_info->var_info.var()->tensor();
  TF_Tensor* result_tensor =
      tensorflow::TF_TensorFromTensor(*tensor, &cc_status);
  Set_TF_Status_from_Status(status, cc_status);
  return result_tensor;
}

void TF_DeleteVariableInfo(TF_VariableInfo* var_info) {
  if (var_info != nullptr) {
    delete var_info;
  }
}

// ---------------------  Coordination service  --------------------------------
TF_CoordinationServiceAgent* TF_GetCoordinationServiceAgent(
    TF_OpKernelContext* ctx) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelContext*>(ctx);
  return reinterpret_cast<TF_CoordinationServiceAgent*>(
      cc_ctx->coordination_service_agent());
}

bool TF_CoordinationServiceIsInitialized(TF_CoordinationServiceAgent* agent) {
  if (agent == nullptr) return false;
  auto* cc_agent = reinterpret_cast<tsl::CoordinationServiceAgent*>(agent);
  return cc_agent->IsInitialized();
}

void TF_CoordinationServiceInsertKeyValue(const char* key, const char* value,
                                          TF_CoordinationServiceAgent* agent,
                                          TF_Status* status) {
  auto* cc_agent = reinterpret_cast<tsl::CoordinationServiceAgent*>(agent);
  tsl::Status cc_status = cc_agent->InsertKeyValue(key, value);
  tsl::Set_TF_Status_from_Status(status, cc_status);
}

TF_Buffer* TF_CoordinationServiceGetKeyValue(const char* key,
                                             TF_CoordinationServiceAgent* agent,
                                             TF_Status* status) {
  auto* cc_agent = reinterpret_cast<tsl::CoordinationServiceAgent*>(agent);
  auto value = cc_agent->GetKeyValue(key);
  tsl::Set_TF_Status_from_Status(status, value.status());
  if (!value.ok()) {
    return nullptr;
  }
  // Caller is responsible to call `TF_DeleteBuffer` to release the buffer.
  TF_Buffer* result = TF_NewBuffer();
  const std::string& value_str = *value;
  void* data = malloc(value_str.length());
  value_str.copy(static_cast<char*>(data), value_str.length(), 0);
  result->data = data;
  result->length = value_str.length();
  result->data_deallocator = [](void* data, size_t length) { free(data); };
  return result;
}

void TF_CoordinationServiceDeleteKeyValue(const char* key,
                                          TF_CoordinationServiceAgent* agent,
                                          TF_Status* status) {
  auto* cc_agent = reinterpret_cast<tsl::CoordinationServiceAgent*>(agent);
  tsl::Status cc_status = cc_agent->DeleteKeyValue(key);
  tsl::Set_TF_Status_from_Status(status, cc_status);
}

// ----------------------------  PJRT  -----------------------------------------
void TF_CreateAndSetPjRtCApiClient(const char* device_type, TF_Status* status) {
  tsl::StatusOr<std::unique_ptr<xla::PjRtClient>> pjrt_client =
      xla::GetCApiClient(device_type);
  if (!pjrt_client.ok()) {
    tensorflow::Set_TF_Status_from_Status(status, pjrt_client.status());
    return;
  }

  tsl::Status s = tensorflow::SetPjRtClientInTFGlobalResourceManager(
      tensorflow::DeviceType(device_type), std::move(*pjrt_client));
  tsl::Set_TF_Status_from_Status(status, s);
}

PJRT_Client* TF_GetPjRtCClient(const char* device_type, TF_Status* status) {
  tsl::StatusOr<xla::PjRtClient*> pjrt_client =
      tensorflow::GetOrCreatePjRtClient(tensorflow::DeviceType(device_type));
  if (!pjrt_client.ok()) {
    tensorflow::Set_TF_Status_from_Status(status, pjrt_client.status());
    return nullptr;
  }
  auto* pjrt_c_api_client =
      tensorflow::down_cast<xla::PjRtCApiClient*>(*pjrt_client);
  if (pjrt_c_api_client == nullptr) {
    tensorflow::Set_TF_Status_from_Status(
        status, tsl::errors::Internal("PjRtClient for ", device_type,
                                      " is not type PjRtCApiClient"));
    return nullptr;
  }
  TF_SetStatus(status, TF_OK, "");
  return pjrt_c_api_client->pjrt_c_client();
}

PJRT_Buffer* TF_GetPjRtCBuffer(TF_Tensor* c_tensor, TF_Status* status) {
  tensorflow::Tensor tensor;
  auto s = tensorflow::TF_TensorToTensor(c_tensor, &tensor);
  if (!s.ok()) {
    tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  tensorflow::AsyncValueTensor* av_tensor =
      tensorflow::AsyncValueTensor::FromTensor(&tensor);
  if (av_tensor == nullptr || av_tensor->GetBuffer() == nullptr) {
    tensorflow::Set_TF_Status_from_Status(
        status,
        tsl::errors::Internal("Input tensor does not have PjRtBuffer."));
    return nullptr;
  }
  auto* c_api_buffer =
      tensorflow::down_cast<xla::PjRtCApiBuffer*>(av_tensor->GetBuffer().get());
  if (c_api_buffer == nullptr) {
    tensorflow::Set_TF_Status_from_Status(
        status,
        tsl::errors::Internal(
            "The PjRtBuffer in the tensor is not type PjRtCApiBuffer."));
    return nullptr;
  }
  TF_SetStatus(status, TF_OK, "");
  return c_api_buffer->c_buffer();
}

void TF_CreatePjRtBuffer(TF_Tensor* c_tensor, PJRT_Buffer* c_buffer,
                         const char* device_type, TF_Status* status) {
  tensorflow::Tensor tensor;
  auto s = tensorflow::TF_TensorToTensor(c_tensor, &tensor);
  if (!s.ok()) {
    tensorflow::Set_TF_Status_from_Status(status, s);
    return;
  }
  auto pjrt_client =
      tensorflow::GetOrCreatePjRtClient(tensorflow::DeviceType(device_type));
  if (!pjrt_client.ok()) {
    tensorflow::Set_TF_Status_from_Status(status, pjrt_client.status());
    return;
  }
  auto* pjrt_c_api_client =
      tensorflow::down_cast<xla::PjRtCApiClient*>(*pjrt_client);
  if (pjrt_c_api_client == nullptr) {
    tensorflow::Set_TF_Status_from_Status(
        status, tsl::errors::Internal("PjRtClient for ", device_type,
                                      " is not type PjRtCApiClient"));
    return;
  }
  tensorflow::AsyncValueTensor* av_tensor =
      tensorflow::AsyncValueTensor::FromTensor(&tensor);
  av_tensor->SetBuffer(
      std::make_unique<xla::PjRtCApiBuffer>(pjrt_c_api_client, c_buffer));
  TF_SetStatus(status, TF_OK, "");
}
