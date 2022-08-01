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

#include "tensorflow/compiler/mlir/tensorflow/utils/serialize_mlir_module_utils.h"
#include "tensorflow/compiler/xla/pjrt/c/pjrt_c_api.h"
// TODO(skyewm): remove when everything goes through C API
#include "tensorflow/compiler/xla/pjrt/c/pjrt_c_api_wrapper_impl.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/tpu/pjrt_api.h"

// TODO(b/238999986): Remove this when we have decomposed shape.
#include "tensorflow/stream_executor/tpu/c_api_conversions.h"

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
    std::unique_ptr<PjRtCApiDevice>& cpp_device = owned_devices_.emplace_back(
        std::make_unique<PjRtCApiDevice>(device, this));
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
    const PjRtLoadedExecutable& executable) const {
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

static Status ValidateCompileOption(CompileOptions options) {
  if (options.argument_layouts.has_value()) {
    return xla::Unimplemented(
        "argument_layouts in CompileOptions is not supported.");
  }
  if (options.compile_portable_executable) {
    return xla::Unimplemented(
        "compile_portable_executable in CompileOptions is not supported.");
  }
  if (options.profile_version != 0) {
    return xla::Unimplemented(
        "profile_version in CompileOptions is not supported.");
  }
  if (options.multi_slice_config != nullptr) {
    return xla::Unimplemented(
        "multi_slice_config in CompileOptions is not supported.");
  }
  return xla::OkStatus();
}

// Convert `CompileOptions` to `PJRT_CompileOptions`. `device_assignment_str`
// will be used for serialized DeviceAssignment storage.
static StatusOr<PJRT_CompileOptions> ConvertCppCompileOptionsToCCompileOptions(
    CompileOptions options, std::string* device_assignment_str) {
  PJRT_CompileOptions c_options;
  c_options.struct_size = PJRT_CompileOptions_STRUCT_SIZE;
  c_options.parameter_is_tupled_arguments =
      options.parameter_is_tupled_arguments;
  c_options.device_ordinal = options.executable_build_options.device_ordinal();
  c_options.num_replicas = options.executable_build_options.num_replicas();
  c_options.num_partitions = options.executable_build_options.num_partitions();
  c_options.use_spmd_partitioning =
      options.executable_build_options.use_spmd_partitioning();
  c_options.allow_spmd_sharding_propagation_to_output =
      options.executable_build_options
          .allow_spmd_sharding_propagation_to_output();

  if (options.executable_build_options.has_device_assignment()) {
    DeviceAssignmentProto device_assignment_proto;
    TF_RETURN_IF_ERROR(
        options.executable_build_options.device_assignment().Serialize(
            &device_assignment_proto));
    *device_assignment_str = device_assignment_proto.SerializeAsString();
    c_options.device_assignment = device_assignment_str->c_str();
    c_options.device_assignment_size = device_assignment_str->size();
  } else {
    c_options.device_assignment_size = 0;
    c_options.device_assignment = nullptr;
  }
  return c_options;
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>> PjRtCApiClient::Compile(
    mlir::ModuleOp module, CompileOptions options) {
  TF_RETURN_IF_ERROR(ValidateCompileOption(options));
  PJRT_Client_Compile_Args args;
  args.struct_size = PJRT_Client_Compile_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.client = c_client_.get();
  std::string device_assignment_str;
  TF_ASSIGN_OR_RETURN(PJRT_CompileOptions c_options,
                      ConvertCppCompileOptionsToCCompileOptions(
                          options, &device_assignment_str));
  args.options = &c_options;
  std::string module_str = tensorflow::SerializeMlirModule(module);
  args.module = module_str.c_str();
  args.module_size = module_str.size();

  RETURN_STATUS_IF_ERROR(c_api_->PJRT_Client_Compile(&args), c_api_);
  std::unique_ptr<PjRtLoadedExecutable> ret =
      std::make_unique<PjRtCApiExecutable>(this, args.executable);
  return ret;
}

StatusOr<std::string> PjRtCApiClient::SerializeExecutable(
    const PjRtLoadedExecutable& executable) const {
  return wrapped_->SerializeExecutable(
      *PjRtCApiExecutable::GetWrapped(&executable));
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>>
PjRtCApiClient::DeserializeExecutable(absl::string_view serialized,
                                      CompileOptions options) {
  return WrapExecutable(wrapped_->DeserializeExecutable(serialized, options));
}

StatusOr<std::uintptr_t> PjRtCApiClient::UnsafeBufferPointer(
    PjRtBuffer* buffer) {
  return wrapped_->UnsafeBufferPointer(PjRtCApiBuffer::GetWrapped(buffer));
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>> PjRtCApiClient::WrapExecutable(
    StatusOr<std::unique_ptr<PjRtLoadedExecutable>> to_wrap) {
  TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtLoadedExecutable> executable,
                      std::move(to_wrap));
  return std::unique_ptr<PjRtLoadedExecutable>(
      std::make_unique<PjRtCApiExecutable>(this, std::move(executable)));
}

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiClient::WrapBuffer(
    StatusOr<std::unique_ptr<PjRtBuffer>> to_wrap) {
  TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtBuffer> buffer, std::move(to_wrap));
  return std::unique_ptr<PjRtBuffer>(std::make_unique<PjRtCApiBuffer>(
      this, new PJRT_Buffer{std::move(buffer), pjrt_c_client()}));
}

const PJRT_Api* PjRtCApiClient::pjrt_c_api() const { return c_api_; }

// --------------------------------- Devices -----------------------------------

PjRtCApiDevice::PjRtCApiDevice(PJRT_Device* device, PjRtCApiClient* client)
    : client_(client), device_(device) {
  wrapped_ = device_->device;
  InitAttributes();
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

void PjRtCApiDevice::InitAttributes() {
  attributes_ = {};
  PJRT_Device_Attributes_Args args;
  args.struct_size = PJRT_Device_Attributes_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_Attributes(&args), api);

  for (int i = 0; i < args.num_attributes; ++i) {
    const auto& attribute = args.attributes[i];
    std::string attribute_name(attribute.name, attribute.name_size);
    switch (attribute.type) {
      case PJRT_Device_Attribute::PJRT_Device_Attribute_kString: {
        std::string string_value(attribute.string_value, attribute.value_size);
        attributes_[attribute_name] = PjRtDeviceAttribute(string_value);
        break;
      }
      case PJRT_Device_Attribute::PJRT_Device_Attribute_kInt64: {
        attributes_[attribute_name] =
            PjRtDeviceAttribute(attribute.int64_value);
        break;
      }
      case PJRT_Device_Attribute::PJRT_Device_Attribute_kInt64List: {
        const int64_t* array_ptr(attribute.int64_array_value);
        std::vector<int64_t> int64_array(array_ptr,
                                         array_ptr + attribute.value_size);
        attributes_[attribute_name] = PjRtDeviceAttribute(int64_array);
        break;
      }
    }
  }
}

const absl::flat_hash_map<std::string, PjRtDeviceAttribute>&
PjRtCApiDevice::Attributes() const {
  return attributes_;
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

int PjRtCApiDevice::local_hardware_id() const {
  PJRT_Device_LocalHardwareId_Args args;
  args.struct_size = PJRT_Device_LocalHardwareId_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Device_LocalHardwareId(&args), api);
  return args.local_hardware_id;
}

absl::string_view PjRtCApiDevice::DebugString() const {
  PJRT_Device_DebugString_Args args;
  args.struct_size = PJRT_Device_DebugString_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.device = device_;
  const PJRT_Api* c_api = client_->pjrt_c_api();
  pjrt::LogFatalIfPjrtError(c_api->PJRT_Device_DebugString(&args), c_api);
  absl::string_view debug_string(args.debug_string, args.debug_string_size);
  return debug_string;
}

// ------------------------------- Executables ---------------------------------

PjRtCApiExecutable::PjRtCApiExecutable(
    PjRtCApiClient* client, std::unique_ptr<PjRtLoadedExecutable> wrapped)
    : client_(client),
      executable_(
          new PJRT_Executable{std::move(wrapped), client->pjrt_c_client()}) {
  InitDevices();
}

PjRtCApiExecutable::PjRtCApiExecutable(PjRtCApiClient* client,
                                       PJRT_Executable* executable)
    : client_(client), executable_(executable) {
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

static std::vector<std::vector<PJRT_Buffer*>> Convert2DCppBuffersToCBuffers(
    absl::Span<const std::vector<PjRtBuffer*>> cpp_lists) {
  std::vector<std::vector<PJRT_Buffer*>> c_lists;
  c_lists.reserve(cpp_lists.size());
  for (const auto& cpp_list : cpp_lists) {
    auto& c_list = c_lists.emplace_back();
    c_list.reserve(cpp_list.size());
    for (PjRtBuffer* buffer : cpp_list) {
      auto* c_api_argument = tensorflow::down_cast<PjRtCApiBuffer*>(buffer);
      c_list.push_back(c_api_argument->c_buffer());
    }
  }
  return c_lists;
}

static std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>
Convert2DCBuffersToCppBuffers(PJRT_Buffer*** c_lists, size_t outer_size,
                              int inner_size, xla::PjRtCApiClient* client) {
  std::vector<std::vector<std::unique_ptr<PjRtBuffer>>> ret;
  for (size_t i = 0; i < outer_size; ++i) {
    auto& output_list = ret.emplace_back();
    output_list.reserve(inner_size);
    for (size_t j = 0; j < inner_size; ++j) {
      output_list.push_back(
          std::make_unique<PjRtCApiBuffer>(client, c_lists[i][j]));
    }
  }
  return ret;
}

// TODO(jieying): expose a C API PJRT_Executable_NumOutputs which gets the
// number of putputs from the HloModule inside the implementation.
static StatusOr<int> GetNumOutputsPerDevice(
    const PjRtCApiExecutable& executable, int num_devices) {
  TF_ASSIGN_OR_RETURN(std::vector<std::shared_ptr<HloModule>> hlo_modules,
                      executable.GetHloModules());
  if (hlo_modules.empty()) {
    return xla::InvalidArgument("Hlo modules is empty for executable %s.",
                                executable.name());
  }
  if (hlo_modules.size() != 1) {
    return xla::Unimplemented(
        "MPMD execution not supported by PjRtCApiClient::Execute.");
  }
  xla::Shape shape = hlo_modules[0].get()->result_shape();
  if (shape.IsTuple()) {
    return shape.tuple_shapes_size();
  }
  // The output size is 1 is it is not a tuple.
  return 1;
}

StatusOr<std::vector<std::vector<std::unique_ptr<PjRtBuffer>>>>
PjRtCApiExecutable::Execute(
    absl::Span<const std::vector<PjRtBuffer*>> argument_handles,
    const ExecuteOptions& options,
    std::optional<std::vector<PjRtFuture<Status>>>& returned_futures) {
  PJRT_Executable_Execute_Args args;
  args.struct_size = PJRT_Executable_Execute_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.executable = executable_;
  PJRT_ExecuteOptions c_options;
  args.options = &c_options;
  args.options->struct_size = PJRT_ExecuteOptions_STRUCT_SIZE;
  args.options->launch_id = options.launch_id;
  args.num_devices = argument_handles.size();
  CHECK_GT(args.num_devices, 0);
  args.num_args = argument_handles[0].size();

  // Populates `args.argument_lists` from `argument_handles`.
  std::vector<std::vector<PJRT_Buffer*>> c_argument_lists =
      Convert2DCppBuffersToCBuffers(argument_handles);
  std::vector<PJRT_Buffer**> c_arguments;
  c_arguments.reserve(c_argument_lists.size());
  for (auto& argument_list : c_argument_lists) {
    c_arguments.push_back(argument_list.data());
  }
  args.argument_lists = c_arguments.data();

  // Allocates memory for output. `c_buffer_lists_holder` and `c_buffer_lists`
  // needs to stay alive during the call of `PJRT_Executable_Execute`.
  TF_ASSIGN_OR_RETURN(int num_outputs_per_device,
                      GetNumOutputsPerDevice(*this, args.num_devices));
  size_t outer_size = args.num_devices;
  size_t inner_size = num_outputs_per_device;
  std::vector<std::vector<PJRT_Buffer*>> c_buffer_lists_holder(outer_size);
  auto c_buffer_lists = std::vector<PJRT_Buffer**>(outer_size);
  for (int i = 0; i < outer_size; ++i) {
    c_buffer_lists_holder[i].resize(inner_size);
    c_buffer_lists[i] = c_buffer_lists_holder[i].data();
  }
  args.output_lists = c_buffer_lists.data();

  RETURN_STATUS_IF_ERROR(pjrt_c_api()->PJRT_Executable_Execute(&args),
                         pjrt_c_api());

  return Convert2DCBuffersToCppBuffers(args.output_lists, args.num_devices,
                                       num_outputs_per_device, client_);
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
        client_, new PJRT_Buffer{std::move(buffer), client_->pjrt_c_client()});
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
        client_, new PJRT_Buffer{std::move(buffer), client_->pjrt_c_client()});
  }
  return out;
}

PjRtLoadedExecutable* PjRtCApiExecutable::wrapped() const {
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
    : client_(client), buffer_(buffer), wrapped_(buffer->buffer.get()) {
  set_shape();
}

PjRtCApiBuffer::~PjRtCApiBuffer() { delete buffer_; }

const Shape& PjRtCApiBuffer::on_device_shape() const {
  CHECK(shape_.has_value())
      << "Shape should be initialized in PjRtCApiBuffer constructor.";
  return shape_.value();
}

void PjRtCApiBuffer::set_shape() {
  PJRT_Buffer_OnDeviceTrimmedShape_Args args;
  args.struct_size = PJRT_Buffer_OnDeviceTrimmedShape_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_;

  pjrt::LogFatalIfPjrtError(
      client_->pjrt_c_api()->PJRT_Buffer_OnDeviceTrimmedShape(&args),
      client_->pjrt_c_api());

  xla::PrimitiveType element_type =
      static_cast<xla::PrimitiveType>(args.element_type);

  CHECK_NE(element_type, xla::PrimitiveType::TUPLE);

  absl::Span<const int64_t> dims = ApiConverter::MakeSpan(args.dimensions);
  absl::Span<const bool> dynamic_dims =
      ApiConverter::MakeSpan(args.dynamic_dimensions);

  Shape trimmed_shape = Shape(element_type, dims, dynamic_dims, {});

  if (args.layout.format != xla::INVALID_FORMAT) {
    *(trimmed_shape.mutable_layout()) = ApiConverter::FromC(&args.layout);
  }

  shape_ = trimmed_shape;

  // TODO(amangu): Refactor the deletion.
  if (args.dimensions.size > TPU_C_API_MAX_INLINED) {
    delete[] args.dimensions.heap;
  }

  if (args.dynamic_dimensions.size > TPU_C_API_MAX_INLINED) {
    delete[] args.dynamic_dimensions.heap;
  }

  if (args.layout.format != xla::INVALID_FORMAT) {
    if (args.layout.minor_to_major.size > TPU_C_API_MAX_INLINED) {
      delete[] args.layout.minor_to_major.heap;
    }

    if (args.layout.tiles.size > TPU_C_API_MAX_INLINED) {
      delete[] args.layout.tiles.heap;
    }
  }
}

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

PjRtDevice* PjRtCApiBuffer::device() const {
  PJRT_Buffer_Device_Args args;
  args.struct_size = PJRT_Buffer_Device_Args_STRUCT_SIZE;
  args.priv = nullptr;
  args.buffer = buffer_;
  const PJRT_Api* api = pjrt_c_api();
  pjrt::LogFatalIfPjrtError(api->PJRT_Buffer_Device(&args), api);
  return client_->GetCppDevice(args.device);
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

StatusOr<std::unique_ptr<PjRtBuffer>> PjRtCApiBuffer::CopyToDevice(
    PjRtDevice* dst_device) {
  if (dst_device->client() == client_) {
    PJRT_Buffer_CopyToDevice_Args args;
    args.struct_size = PJRT_Buffer_CopyToDevice_Args_STRUCT_SIZE;
    args.priv = nullptr;
    args.buffer = buffer_;
    args.dst_device =
        tensorflow::down_cast<PjRtCApiDevice*>(dst_device)->c_device();
    const PJRT_Api* api = pjrt_c_api();
    RETURN_STATUS_IF_ERROR(api->PJRT_Buffer_CopyToDevice(&args), api);
    return std::unique_ptr<PjRtBuffer>(
        std::make_unique<PjRtCApiBuffer>(client_, args.dst_buffer));
  } else {
    // TODO(b/239735405) Copying across different clients where `dst_device` is
    // not a PjRtCApiDevice raises an error.
    return wrapped_->CopyToDevice(dst_device);
  }
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
