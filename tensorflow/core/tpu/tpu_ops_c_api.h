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
#ifndef TENSORFLOW_CORE_TPU_KERNELS_TPU_KERNELS_C_API_H_
#define TENSORFLOW_CORE_TPU_KERNELS_TPU_KERNELS_C_API_H_

#include <stddef.h>

#include <cstdint>

#include "tensorflow/core/tpu/libtftpu.h"
#include "tensorflow/stream_executor/tpu/c_api_decl.h"
#include "tensorflow/stream_executor/tpu/proto_helper.h"

typedef struct TpuSerializedProto TpuSerializedProto;

namespace tensorflow {
class TpuMeshCommonState;
}  // namespace tensorflow

extern "C" {

typedef struct XLA_TpuProgram XLA_TpuProgram;

// Enum for choosing sharding/unsharding program from a `XLA_TpuProgram` obj.
enum TpuProgramShardingType { kInvalid = 0, kMain, kSharding, kUnsharding };

struct TpuExecutableSerializedProto {
  const char* bytes;
  size_t size;
};

struct CompilerMetadataSerializedProto {
  const char* bytes;
  size_t size;
};

struct HostComputeMetadataSerializedProto {
  const char* bytes;
  size_t size;
};

typedef struct XLA_TpuMeshState XLA_TpuMeshState;

typedef struct XLA_DeviceAssignment {
  const char* bytes;
  size_t size;
} XLA_DeviceAssignment;

// Property for creating compilation cache key.
struct CompilationCacheKeyProperty {
  const char* config_prefix;
  const char* shapes_prefix;
  const char* function_name;
  uint64_t mlir_module_fingerprint;
  const int32_t* device_ids;
  size_t device_ids_size;
  int32_t guaranteed_constants_size;
  uint64_t function_library_fingerprint;
  int32_t num_cores_per_replica;
  int32_t num_replicas;
  const XLA_TpuMeshState* mesh_state;
};

// Compilation cache key result returning both the key and a more verbose debug
// version.
struct CompilationCacheKeyResult {
  const char* key;
  const char* debug_string;
};

typedef struct XLA_TpuNodeContext XLA_TpuNodeContext;

// Compiles Mlir or TF function computation by lowering into HLO IR and returns
// `count` number of TPU programs ready for execution.
// The API allocates the `XLA_TpuProgram*[]` array `tpu_programs` and creates
// `XLA_TpuProgram` object(s) using the `TpuProgram_New` API. The caller is
// responsible to deallocate both the `XLA_TpuProgram*[]` array and the
// `XLA_TpuProgram` object(s) using `TpuProgram_FreeArray` and `TpuProgram_Free`
// API respectively.
TFTPU_CAPI_EXPORT void TpuCompile_CompileAndBuild(
    TpuSerializedProto compilation_request, const XLA_TpuMeshState* mesh_state,
    XLA_TpuProgram** tpu_programs[], size_t* count, TF_Status* status);

// Creates a new TPU mesh state object.
TFTPU_CAPI_EXPORT XLA_TpuMeshState* TpuMeshState_Create();

// Deletes the given TPU `mesh_state` object. Once deleted the object is
// unusable.
TFTPU_CAPI_EXPORT void TpuMeshState_Free(XLA_TpuMeshState* mesh_state);

// Returns a pointer to an opaque mesh data structure used internally.
TFTPU_CAPI_EXPORT void* TpuMeshState_MeshCommonState(
    XLA_TpuMeshState* mesh_state);

TFTPU_CAPI_EXPORT void TpuExecutable_LoadProgramAndEnqueueToStream(
    const XLA_TpuProgram* program, SE_DeviceMemoryBase* arguments,
    size_t arguments_len, SE_DeviceMemoryBase* result,
    SE_DeviceMemoryBase* cross_program_prefetch_addr, int32_t rng_seed,
    XLA_DeviceAssignment* device_assignment, SE_Stream* stream,
    TF_Status* status);

TFTPU_CAPI_EXPORT void HardwareLayout_HostShapeToDeviceShape(
    XLA_Shape* host_shape, XLA_Shape* device_shape);
TFTPU_CAPI_EXPORT int64_t HardwareLayout_ShapeSize(XLA_Shape* shape);
TFTPU_CAPI_EXPORT int64_t HardwareLayout_ShapeSizeCompact(XLA_Shape* shape);
TFTPU_CAPI_EXPORT int64_t HardwareLayout_ShapeSizeCompactRaw(XLA_Shape* shape);

TFTPU_CAPI_EXPORT void TpuExecute_RuntimeInputToPaddedData(
    uint32_t* runtime_input_ptr, size_t runtime_input_size,
    int8_t* padded_data_ptr, size_t padded_data_size, XLA_Shape* runtime_shape,
    XLA_Shape* compile_time_shape, TF_Status* status);

TFTPU_CAPI_EXPORT void ConfigureDistributedTpuOp_DoWork(
    const size_t num_cores_per_host_size, const int32_t* num_cores_per_host,
    size_t server_address_size, const char* server_address,
    size_t* host_config_output_size, char** host_config_output,
    TF_Status* status);

TFTPU_CAPI_EXPORT void WaitForDistributedTpuOp_DoWork(
    const size_t num_hosts, const size_t num_cores_per_host,
    const int32_t** host_ordinal_to_global_core_id_map,
    tensorflow::TpuMeshCommonState* tpu_mesh_common_state,
    size_t* tpu_topology_output_size, char** tpu_topology_output,
    TF_Status* status);

TFTPU_CAPI_EXPORT void InitializeHostForDistributedTpuOp_DoWork(
    const size_t tpu_host_config_size, const char* tpu_host_config,
    const bool enable_whole_mesh_compilations, bool is_master_worker,
    size_t* core_id_output_size, int32_t** core_id_output, TF_Status* status);

TFTPU_CAPI_EXPORT void SetGlobalTPUArrayOp_DoWork(
    const size_t tpu_topology_size, const char* tpu_topology,
    TF_Status* status);

TFTPU_CAPI_EXPORT void DisconnectDistributedTpuChipsOp_DoWork(
    int32_t* number_of_chips_output, TF_Status* status);

TFTPU_CAPI_EXPORT void TpuConfigurationApi_FreeCharArray(char* output);
TFTPU_CAPI_EXPORT void TpuConfigurationApi_FreeInt32Array(int32_t* output);

TFTPU_CAPI_EXPORT bool TpuConfigurationApi_HasTPUPodState();

TFTPU_CAPI_EXPORT void TpuConfigurationApi_TpusPerHost(int32_t* tpus,
                                                       TF_Status* status);
TFTPU_CAPI_EXPORT void TpuConfigurationApi_TpuMemoryLimit(int64_t* memory_limit,
                                                          TF_Status* status);
TFTPU_CAPI_EXPORT void TpuConfigurationApi_RemoteCompilationCacheSizeInBytes(
    int64_t* cache_size_in_bytes);
TFTPU_CAPI_EXPORT
void TpuConfigurationApi_CompilationCacheServerAddressFromConfig(
    size_t tpu_host_config_size, const char* tpu_host_config,
    size_t* server_address_output_size, char** server_address_output,
    TF_Status* status);
TFTPU_CAPI_EXPORT void TpuConfigurationApi_GetServerAddressAndPort(
    size_t* server_address_output_size, char** server_address_output,
    int* port_output, TF_Status* status);

// Creates a new TPU program.
TFTPU_CAPI_EXPORT XLA_TpuProgram* TpuProgram_New();

// Destroys the `tpu_program`.
TFTPU_CAPI_EXPORT void TpuProgram_Free(XLA_TpuProgram* tpu_program);

// Creates an array of `XLA_TpuProgram*`.
TFTPU_CAPI_EXPORT XLA_TpuProgram** TpuProgram_NewArray(size_t count);

// Destroys an array of `XLA_TpuProgram*`.
TFTPU_CAPI_EXPORT void TpuProgram_FreeArray(XLA_TpuProgram* tpu_program[]);

// Unloads and destroys the `tpu_program`. Once the TPU program is unloaded and
// destroyed, it is in an unusable state.
TFTPU_CAPI_EXPORT void TpuProgram_UnloadAndDestroy(XLA_TpuProgram* tpu_program,
                                                   TF_Status* status);

// Gets TPU program size in bytes from the `tpu_program`.
TFTPU_CAPI_EXPORT int64_t
TpuProgram_GetProgramSize(const XLA_TpuProgram* tpu_program);

// Logs the summary of current memory state snapshot of the `tpu_program`.
TFTPU_CAPI_EXPORT bool TpuProgram_LogProgramMemorySummary(
    const XLA_TpuProgram* tpu_program);

// Gets TPU program executable info from the `tpu_program`.
TFTPU_CAPI_EXPORT void TpuProgram_GetExecutableInfo(
    const XLA_TpuProgram* tpu_program, TpuSerializedProto* executable_info,
    TF_Status* status);

// Gets host transfer info proto.
TFTPU_CAPI_EXPORT void TpuProgram_GetHostTransferInfo(
    const XLA_TpuProgram* tpu_program, TpuSerializedProto* host_transfer_info,
    TF_Status* status);

// Gets HLO metadata proto.
TFTPU_CAPI_EXPORT void TpuProgram_GetHloMetadata(
    const XLA_TpuProgram* tpu_program, TpuSerializedProto* hlo_metadata,
    TF_Status* status);

// Gets may modify variables boolean value.
TFTPU_CAPI_EXPORT void TpuProgram_GetMayModifyVariables(
    const XLA_TpuProgram* tpu_program, bool* may_modify_variables);

// Checks if TPU program has sharding.
TFTPU_CAPI_EXPORT bool TpuProgram_HasSharding(
    const XLA_TpuProgram* tpu_program);

// Gets TPU program by sharding type. Return value is valid only when the
// `status.status()` returns `OK`.
TFTPU_CAPI_EXPORT XLA_TpuProgram* TpuProgram_GetTpuProgram(
    XLA_TpuProgram* tpu_program, TpuProgramShardingType type);

// Gets TPU executable proto from a `tpu_program`.
TFTPU_CAPI_EXPORT void TpuProgram_SerializeTpuExecutable(
    const XLA_TpuProgram* tpu_program, TpuExecutableSerializedProto* executable,
    TF_Status* status);

// Gets compilation metadata proto from a `tpu_program`.
TFTPU_CAPI_EXPORT void TpuProgram_SerializeCompilerMetadata(
    const XLA_TpuProgram* tpu_program,
    CompilerMetadataSerializedProto* compiler_metadata, TF_Status* status);

// Deserializes the `GetTpuProgramResponse` proto into an `XLA_TpuProgram`.
TFTPU_CAPI_EXPORT void TpuProgram_DeserializeFromGetTpuProgramResponseProto(
    TpuSerializedProto get_tpu_program_response, XLA_TpuProgram* tpu_program,
    TF_Status* status);

// Checks if whether a TPU compilation is enabled.
TFTPU_CAPI_EXPORT bool TpuCompile_IsTpuCompilationEnabled();

// XLA compilation cannot be cancelled. To avoid hanging the TF worker will exit
// when cancellation is requested for an XLA compile op. Some tests require this
// behavior to be disabled, and we test for this condition with the following
// flag function.
TFTPU_CAPI_EXPORT bool TpuCompile_ShouldTpuCompileOpIgnoreCancellation();

// Returns the number of available TPU core count.
TFTPU_CAPI_EXPORT int TpuTopology_AvailableCoreCount(
    const XLA_TpuMeshState* mesh_state, TpuCoreTypeEnum tpu_core_type);

// Recycle unused service port.
TFTPU_CAPI_EXPORT void TpuNetUtil_RecycleUnusedPort(int port);

// Creates a unique compilation cache `key` used for `put` and `get` operations.
// Returned buffers are heap-allocated and must be owned.
TFTPU_CAPI_EXPORT CompilationCacheKeyResult
TpuCompile_CreateCompilationCacheKey(CompilationCacheKeyProperty property);

// Destroys the CompilationCacheKeyResult returned by calling the
// `TpuCompile_CreateCompilationCacheKey` API.
TFTPU_CAPI_EXPORT void TpuCompile_DestroyCompilationCacheKey(
    CompilationCacheKeyResult result);

// Creates a guaranteed const fingerprint. Guarantee const is normally used in
// TPU inference to avoid re-copying unchanged variables onto the TPU device.
// It promises the value is identical for every execution in the same session
// even if the actual value changes in later executions.
TFTPU_CAPI_EXPORT uint64_t TpuCompile_CreateGuaranteedConstFingerprint(
    uint64_t fingerprint, const char* data, size_t size);

XLA_TpuNodeContext* TpuNodeContext_Create(int device_ordinal,
                                          TF_Status* status);
void TpuNodeContext_Free(XLA_TpuNodeContext* node_context);

void TpuNodeContext_StopChipHeartbeats(TF_Status* status);

void TpuNodeContext_CloseTpuHost(TF_Status* status);

void TpuNodeContext_Initialize(int device_ordinal, TF_Status* status);

struct TfTpu_OpsApiFn {
  TFTPU_ADD_FN_IN_STRUCT(TpuCompile_CompileAndBuild);

  TFTPU_ADD_FN_IN_STRUCT(TpuMeshState_Create);
  TFTPU_ADD_FN_IN_STRUCT(TpuMeshState_Free);
  TFTPU_ADD_FN_IN_STRUCT(TpuMeshState_MeshCommonState);

  TFTPU_ADD_FN_IN_STRUCT(TpuExecutable_LoadProgramAndEnqueueToStream);
  TFTPU_ADD_FN_IN_STRUCT(HardwareLayout_HostShapeToDeviceShape);
  TFTPU_ADD_FN_IN_STRUCT(HardwareLayout_ShapeSize);
  TFTPU_ADD_FN_IN_STRUCT(HardwareLayout_ShapeSizeCompact);
  TFTPU_ADD_FN_IN_STRUCT(HardwareLayout_ShapeSizeCompactRaw);
  TFTPU_ADD_FN_IN_STRUCT(TpuExecute_RuntimeInputToPaddedData);

  TFTPU_ADD_FN_IN_STRUCT(ConfigureDistributedTpuOp_DoWork);
  TFTPU_ADD_FN_IN_STRUCT(WaitForDistributedTpuOp_DoWork);
  TFTPU_ADD_FN_IN_STRUCT(InitializeHostForDistributedTpuOp_DoWork);
  TFTPU_ADD_FN_IN_STRUCT(SetGlobalTPUArrayOp_DoWork);
  TFTPU_ADD_FN_IN_STRUCT(DisconnectDistributedTpuChipsOp_DoWork);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_FreeCharArray);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_FreeInt32Array);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_HasTPUPodState);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_TpusPerHost);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_TpuMemoryLimit);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_RemoteCompilationCacheSizeInBytes);
  TFTPU_ADD_FN_IN_STRUCT(
      TpuConfigurationApi_CompilationCacheServerAddressFromConfig);
  TFTPU_ADD_FN_IN_STRUCT(TpuConfigurationApi_GetServerAddressAndPort);

  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_New);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_Free);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_NewArray);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_FreeArray);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_UnloadAndDestroy);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_GetProgramSize);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_LogProgramMemorySummary);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_GetExecutableInfo);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_GetHostTransferInfo);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_GetHloMetadata);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_GetMayModifyVariables);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_HasSharding);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_GetTpuProgram);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_SerializeTpuExecutable);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_SerializeCompilerMetadata);
  TFTPU_ADD_FN_IN_STRUCT(TpuProgram_DeserializeFromGetTpuProgramResponseProto);

  TFTPU_ADD_FN_IN_STRUCT(TpuCompile_IsTpuCompilationEnabled);
  TFTPU_ADD_FN_IN_STRUCT(TpuCompile_ShouldTpuCompileOpIgnoreCancellation);
  TFTPU_ADD_FN_IN_STRUCT(TpuTopology_AvailableCoreCount);
  TFTPU_ADD_FN_IN_STRUCT(TpuNetUtil_RecycleUnusedPort);
  TFTPU_ADD_FN_IN_STRUCT(TpuCompile_CreateCompilationCacheKey);
  TFTPU_ADD_FN_IN_STRUCT(TpuCompile_DestroyCompilationCacheKey);
  TFTPU_ADD_FN_IN_STRUCT(TpuCompile_CreateGuaranteedConstFingerprint);

  TFTPU_ADD_FN_IN_STRUCT(TpuNodeContext_Create);
  TFTPU_ADD_FN_IN_STRUCT(TpuNodeContext_Free);
  TFTPU_ADD_FN_IN_STRUCT(TpuNodeContext_StopChipHeartbeats);
  TFTPU_ADD_FN_IN_STRUCT(TpuNodeContext_CloseTpuHost);
  TFTPU_ADD_FN_IN_STRUCT(TpuNodeContext_Initialize);
};

}  // extern "C"

#endif  // TENSORFLOW_CORE_TPU_KERNELS_TPU_KERNELS_C_API_H_
