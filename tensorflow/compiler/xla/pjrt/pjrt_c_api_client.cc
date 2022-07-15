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

#include "tensorflow/compiler/xla/pjrt/pjrt_c_api_client.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/pjrt/c/pjrt_c_api.h"
// TODO(skyewm): remove when everything goes through C API
#include "tensorflow/compiler/xla/pjrt/c/pjrt_c_api_wrapper_impl.h"
#include "tensorflow/core/tpu/pjrt_api.h"

namespace xla {

// Helper macros

// Return error status if not success and frees the PJRT_Error returned by
// `expr`.
#define RETURN_STATUS_IF_ERROR(expr, c_api)                             \
  do {                                                                  \
    PJRT_Error* error = (expr);                                         \
    std::unique_ptr<PJRT_Error, pjrt::PJRT_ErrorDeleter> _error(        \
        error, pjrt::MakeErrorDeleter(c_api));                          \
    xla::Status _status = pjrt::PjrtErrorToStatus(_error.get(), c_api); \
    if (!_status.ok()) {                                                \
      return _status;                                                   \
    }                                                                   \
  } while (false)

// ---------------------------------- Client -----------------------------------

PjRtCApiClient::PjRtCApiClient(const PJRT_Api* c_api, PJRT_Client* c_client)
    : c_api_(c_api),
      c_client_(std::unique_ptr<PJRT_Client, ::pjrt::PJRT_ClientDeleter>(
          c_client, ::pjrt::MakeClientDeleter(c_api))) {
  wrapped_ = c_client_->client.get();

  InitDevices();
}

void PjRtCApiClient::InitDevices() {
  PJRT_Client_Devices_Args devices_args;
  devices_args.struct_size = PJRT_Client_Devices_Args_STRUCT_SIZE;
  devices_args.priv = nullptr;
  devices_args.client = c_client_.get();

  pjrt::LogFatalIfPjrtError(c_api_->PJRT_Client_Devices(&devices_args), c_api_);

  const size_t n = devices_args.num_devices;
  wrapped_device_map_.reserve(n);
  c_to_cpp_device_map_.reserve(n);
  owned_devices_.reserve(n);
  devices_.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    PJRT_Device* device = devices_args.devices[i];
    std::unique_ptr<PjRtCApiDevice>& cpp_device =
        owned_devices_.emplace_back(std::make_unique<PjRtCApiDevice>(device));
    cpp_device->SetClient(this);
    devices_.push_back(cpp_device.get());
    c_to_cpp_device_map_[device] = cpp_device.get();
    // Map the wrapped PjRtDevice* to the PjRtCApiDevice* that wraps it.
    // TODO(b/237017893): remove `wrapped_device_map_` and replace it with
    // `c_api_device_map_`
    wrapped_device_map_[device->device] = cpp_device.get();
  }

  PJRT_Client_AddressableDevices_Args address_args;
  address_args.struct_size = PJRT_Client_AddressableDevices_Args_STRUCT_SIZE;
  address_args.priv = nullptr;
  address_args.client = c_client_.get();

  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_Client_AddressableDevices(&address_args), c_api_);

  const size_t m = address_args.num_addressable_devices;
  addressable_devices_.reserve(m);

  for (size_t i = 0; i < m; ++i) {
    PJRT_Device* c_device = address_args.addressable_devices[i];
    addressable_devices_.push_back(GetCppDevice(c_device));
  }
}

int PjRtCApiClient::device_count() const { return devices_.size(); }

int PjRtCApiClient::addressable_device_count() const {
  return addressable_devices_.size();
}

absl::Span<PjRtDevice* const> PjRtCApiClient::devices() const {
  return devices_;
}

absl::Span<PjRtDevice* const> PjRtCApiClient::addressable_devices() const {
  return addressable_devices_;
}

absl::string_view PjRtCApiClient::platform_name() const {
  PJRT_Client_PlatformName_Args args;
  args.client = c_client_.get();
  args.struct_size = PJRT_Client_PlatformName_Args_STRUCT_SIZE;
  args.priv = nullptr;
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_Client_PlatformName(&args), c_api_);

  absl::string_view platform_name(args.platform_name, args.platform_name_size);
  return platform_name;
}

int PjRtCApiClient::process_index() const {
  PJRT_Client_ProcessIndex_Args process_index_args;
  process_index_args.struct_size = PJRT_Client_ProcessIndex_Args_STRUCT_SIZE;
  process_index_args.priv = nullptr;
  process_index_args.client = c_client_.get();
  pjrt::LogFatalIfPjrtError(
      c_api_->PJRT_Client_ProcessIndex(&process_index_args), c_api_);

  return process_index_args.process_index;
}

absl::string_view PjRtCApiClient::platform_version() const {
  PJRT_Client_PlatformVersion_Args args;
  args.struct_size = PJRT_Client_PlatformVersion_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  pjrt::LogFatalIfPjrtError(c_api_->PJRT_Client_PlatformVersion(&args), c_api_);

  absl::string_view platform_version(args.platform_version,
                                     args.platform_version_size);
  return platform_version;
}

StatusOr<std::optional<std::string>> PjRtCApiClient::ExecutableFingerprint(
    const PjRtExecutable& executable) const {
  return wrapped_->ExecutableFingerprint(
      *PjRtCApiExecutable::GetWrapped(&executable));
}

StatusOr<PjRtDevice*> PjRtCApiClient::LookupDevice(int device_id) const {
  PJRT_Client_LookupDevice_Args args;
  args.struct_size = PJRT_Client_LookupDevice_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  args.id = device_id;
  RETURN_STATUS_IF_ERROR(c_api_->PJRT_Client_LookupDevice(&args), c_api_);
  return GetCppDevice(args.device);
}

StatusOr<std::string> PjRtCApiClient::SerializeExecutable(
    const PjRtExecutable& executable) const {
  return wrapped_->SerializeExecutable(
      *PjRtCApiExecutable::GetWrapped(&executable));
}

StatusOr<std::unique_ptr<PjRtExecutable>> PjRtCApiClient::DeserializeExecutable(
    absl::string_view serialized, CompileOptions options) {
  return WrapExecutable(wrapped_->DeserializeExecutable(serialized, options));
}

StatusOr<std::uintptr_t> PjRtCApiClient::UnsafeBufferPointer(
    PjRtBuffer* buffer) {
  return wrapped_->UnsafeBufferPointer(PjRtCApiBuffer::GetWrapped(buffer));
}

StatusOr<std::unique_ptr<PjRtExecutable>> PjRtCApiClient::WrapExecutable(
    StatusOr<std::unique_ptr<PjRtExecutable>> to_wrap) {
  TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtExecutable> executable,
                      std::move(to_wrap));
  return std::unique_ptr<PjRtExecutable>(
      std::make_unique<PjRtCApiExecutable>(this, std::move(executable)));
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiClient::WrapBuffer(
    StatusOr<std::unique_ptr<PjRtBuffer>> to_wrap) {
  TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtBuffer> buffer, std::move(to_wrap));
  return std::unique_ptr<PjRtBuffer>(std::make_unique<PjRtCApiBuffer>(
      this, new PJRT_Buffer{std::move(buffer)}));
}

const PJRT_Api* PjRtCApiClient::pjrt_c_api() const { return c_api_; }

// --------------------------------- Devices -----------------------------------

PjRtCApiDevice::PjRtCApiDevice(PJRT_Device* device) : device_(device) {
  wrapped_ = device_->device;
}

PjRtClient* PjRtCApiDevice::client() const { return client_; }

int PjRtCApiDevice::id() const {
  PJRT_Device_Id_Args args;
  args.struct_size = PJRT_Device_Id_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_Id(&args), api);
  return args.id;
}

int PjRtCApiDevice::process_index() const {
  PJRT_Device_ProcessIndex_Args args;
  args.struct_size = PJRT_Device_ProcessIndex_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_ProcessIndex(&args), api);
  return args.process_index;
}

bool PjRtCApiDevice::IsAddressable() const {
  PJRT_Device_IsAddressable_Args args;
  args.struct_size = PJRT_Device_IsAddressable_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_IsAddressable(&args), api);
  return args.is_addressable;
}

absl::string_view PjRtCApiDevice::device_kind() const {
  PJRT_Device_Kind_Args args;
  args.struct_size = PJRT_Device_Kind_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;

  const PJRT_Api* c_api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Device_Kind(&args), c_api);

  absl::string_view device_kind(args.device_kind, args.device_kind_size);
  return device_kind;
}

// ------------------------------- Executables ---------------------------------

PjRtCApiExecutable::PjRtCApiExecutable(PjRtCApiClient* client,
                                       std::unique_ptr<PjRtExecutable> wrapped)
    : client_(client),
      executable_(
          new PJRT_Executable{std::move(wrapped), client->pjrt_c_client()}) {
  InitDevices();
}

void PjRtCApiExecutable::InitDevices() {
  PJRT_Executable_AddressableDevices_Args args;
  args.struct_size = PJRT_Executable_AddressableDevices_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable_;
  args.addressable_devices = nullptr;
  args.num_addressable_devices = 0;

  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Executable_AddressableDevices(&args),
                            api);

  const size_t num_addressable_devices = args.num_addressable_devices;
  addressable_devices_.reserve(num_addressable_devices);

  for (size_t i = 0; i < num_addressable_devices; ++i) {
    PJRT_Device* device = args.addressable_devices[i];
    PjRtCApiDevice* c_api_device = client_->GetCppDevice(device);
    addressable_devices_.push_back(c_api_device);
  }
}

PjRtCApiExecutable::~PjRtCApiExecutable() {
  PJRT_Executable_Destroy_Args args;
  args.struct_size = PJRT_Executable_Destroy_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable_;
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Executable_Destroy(&args), api);
}

StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
PjRtCApiExecutable::Execute(
    absl::Span<const std::vector<PjRtBuffer*>> argument_handles,
    const ExecuteOptions& options,
    std::optional<std::vector<PjRtFuture<Status>>>& returned_futures) {
  std::vector<std::vector<PjRtBuffer*>> wrapped_args;
  for (const std::vector<PjRtBuffer*>& args : argument_handles) {
    wrapped_args.push_back(PjRtCApiBuffer::GetWrappedVector(args));
  }

  TF_ASSIGN_OR_RETURN(
      std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> out,
      wrapped()->Execute(wrapped_args, options, returned_futures));

  for (auto& buffer_list : out) {
    for (std::unique_ptr<PjRtBuffer>& buffer : buffer_list) {
      buffer = std::make_unique<PjRtCApiBuffer>(
          client_, new PJRT_Buffer{std::move(buffer)});
    }
  }
  return out;
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
PjRtCApiExecutable::ExecuteSharded(
    absl::Span<PjRtBuffer* const> argument_handles, PjRtDevice* device,
    const ExecuteOptions& options,
    std::optional<PjRtFuture<Status>>& returned_future, bool fill_future) {
  std::vector<PjRtBuffer*> wrapped_args =
      PjRtCApiBuffer::GetWrappedVector(argument_handles);

  TF_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<PjRtBuffer>> out,
                      wrapped()->ExecuteSharded(
                          wrapped_args, PjRtCApiDevice::GetWrapped(device),
                          options, returned_future, fill_future));

  for (std::unique_ptr<PjRtBuffer>& buffer : out) {
    buffer = std::make_unique<PjRtCApiBuffer>(
        client_, new PJRT_Buffer{std::move(buffer)});
  }
  return out;
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
PjRtCApiExecutable::ExecutePortable(
    absl::Span<PjRtBuffer* const> argument_handles, PjRtDevice* device,
    const ExecuteOptions& options,
    std::optional<PjRtFuture<Status>>& returned_future, bool fill_future) {
  std::vector<PjRtBuffer*> wrapped_args =
      PjRtCApiBuffer::GetWrappedVector(argument_handles);

  TF_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<PjRtBuffer>> out,
                      wrapped()->ExecutePortable(
                          wrapped_args, PjRtCApiDevice::GetWrapped(device),
                          options, returned_future, fill_future));

  for (std::unique_ptr<PjRtBuffer>& buffer : out) {
    buffer = std::make_unique<PjRtCApiBuffer>(
        client_, new PJRT_Buffer{std::move(buffer)});
  }
  return out;
}

PjRtExecutable* PjRtCApiExecutable::wrapped() const {
  return executable_->executable.get();
}

absl::string_view PjRtCApiExecutable::name() const {
  const PJRT_Api* c_api = pjrt_c_api();
  PJRT_Executable_Name_Args args;
  args.executable = executable_;
  args.struct_size = PJRT_Executable_Name_Args_STRUCT_SIZE;
  args.priv = nullptr;
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Executable_Name(&args), c_api);

  absl::string_view executable_name(args.executable_name,
                                    args.executable_name_size);
  return executable_name;
}

void PjRtCApiExecutable::Delete() {
  PJRT_Executable_Delete_Args args;
  args.struct_size = PJRT_Executable_Delete_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable_;
  const PJRT_Api* c_api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Executable_Delete(&args), c_api);
}

bool PjRtCApiExecutable::IsDeleted() {
  PJRT_Executable_IsDeleted_Args args;
  args.struct_size = PJRT_Executable_IsDeleted_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable_;

  const PJRT_Api* c_api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Executable_IsDeleted(&args), c_api);
  return args.is_deleted;
}

// ---------------------------------- Buffers ----------------------------------

PjRtCApiBuffer::PjRtCApiBuffer(PjRtCApiClient* client, PJRT_Buffer* buffer)
    : client_(client), buffer_(buffer), wrapped_(buffer->buffer.get()) {}

PjRtCApiBuffer::~PjRtCApiBuffer() { delete buffer_; }

StatusOr<size_t> PjRtCApiBuffer::GetOnDeviceSizeInBytes() const {
  PJRT_Buffer_OnDeviceSizeInBytes_Args args;
  args.struct_size = PJRT_Buffer_OnDeviceSizeInBytes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_;
  RETURN_STATUS_IF_ERROR(
      client_->pjrt_c_api()->PJRT_Buffer_OnDeviceSizeInBytes(&args),
      client_->pjrt_c_api());

  return args.on_device_size_in_bytes;
}

void PjRtCApiBuffer::Delete() {
  PJRT_Buffer_Delete_Args args;
  args.struct_size = PJRT_Buffer_Delete_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_;
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_Delete(&args), api);
}

bool PjRtCApiBuffer::IsDeleted() {
  PJRT_Buffer_IsDeleted_Args args;
  args.struct_size = PJRT_Buffer_IsDeleted_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_;
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_IsDeleted(&args), api);
  return args.is_deleted;
}

bool PjRtCApiBuffer::IsOnCpu() const {
  PJRT_Buffer_IsOnCpu_Args args;
  args.struct_size = PJRT_Buffer_IsOnCpu_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_;
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_IsOnCpu(&args), api);
  return args.is_on_cpu;
}

// -------------------------------- API access ---------------------------------

StatusOr<std::unique_ptr<PjRtClient>> GetCApiClient() {
  const PJRT_Api* c_api = tensorflow::tpu::PjrtApi();
  // TODO(skyewm): make status
  CHECK(c_api != nullptr);

  PJRT_Client_Create_Args init_args;
  init_args.struct_size = PJRT_Client_Create_Args_STRUCT_SIZE;
  init_args.priv = nullptr;
  RETURN_STATUS_IF_ERROR(c_api->PJRT_Client_Create(&init_args), c_api);
  PJRT_Client* c_client = init_args.client;

  return std::unique_ptr<PjRtClient>(
      std::make_unique<PjRtCApiClient>(c_api, c_client));
}

}  // namespace xla
