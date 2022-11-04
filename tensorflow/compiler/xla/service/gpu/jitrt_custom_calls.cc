// Copyright 2022 The TensorFlow Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tensorflow/compiler/xla/service/gpu/jitrt_custom_calls.h"

#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/runtime/arguments.h"
#include "tensorflow/compiler/xla/runtime/custom_call.h"
#include "tensorflow/compiler/xla/runtime/custom_call_registry.h"
#include "tensorflow/compiler/xla/runtime/executable.h"
#include "tensorflow/compiler/xla/runtime/jit_executable.h"
#include "tensorflow/compiler/xla/runtime/type_id.h"
#include "tensorflow/compiler/xla/runtime/types.h"
#include "tensorflow/compiler/xla/service/custom_call_status_internal.h"
#include "tensorflow/compiler/xla/service/custom_call_target_registry.h"
#include "tensorflow/compiler/xla/service/gpu/fft_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_asm_opts_util.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_conv_runner.h"
#include "tensorflow/compiler/xla/service/gpu/matmul_utils.h"
#include "tensorflow/compiler/xla/service/gpu/nccl_all_gather_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/nccl_all_reduce_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/nccl_all_to_all_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/nccl_collective_permute_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/nccl_collective_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/cholesky.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/collectives.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/conv.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/cublas_lt_matmul.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/fft.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/gemm.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/io_feed.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/kernel_launch.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/memcpy.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/memset.h"
#include "tensorflow/compiler/xla/service/gpu/runtime/support.h"
#include "tensorflow/compiler/xla/service/gpu/stream_executor_util.h"
#include "tensorflow/compiler/xla/service/service_executable_run_options.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/tsl/platform/human_readable_json.h"

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#include "tensorflow/compiler/xla/service/gpu/cholesky_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/triangular_solve_thunk.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_stream.h"
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

#if GOOGLE_CUDA
#include "tensorflow/compiler/xla/service/gpu/runtime/graph_launch.h"
#endif  // GOOGLE_CUDA

namespace xla {
namespace gpu {

using Eigen::bfloat16;
using Eigen::half;

using llvm::ArrayRef;

using mlir::failure;
using mlir::FailureOr;
using mlir::LogicalResult;
using mlir::StringRef;
using mlir::succeeded;

using ::xla::runtime::AggregateAttrDef;
using ::xla::runtime::AggregateAttrEncoding;
using ::xla::runtime::CustomCall;
using ::xla::runtime::CustomCallAttrEncodingSet;
using ::xla::runtime::EnumAttrEncoding;
using ::xla::runtime::Executable;
using ::xla::runtime::FlatMemrefView;
using ::xla::runtime::StridedMemrefView;
using ::xla::runtime::Tagged;
using ::xla::runtime::TypeIDNameRegistry;

namespace se = ::stream_executor;
namespace lmhlo_gpu = ::mlir::lmhlo_gpu;
namespace mhlo = ::mlir::mhlo;

// Disable all CustomCall checks in optimized build.
static constexpr CustomCall::RuntimeChecks RuntimeChecks() {
#if defined(NDEBUG)
  return CustomCall::RuntimeChecks::kNone;
#else
  return CustomCall::RuntimeChecks::kDefault;
#endif
}

// -------------------------------------------------------------------------- //

// Add custom call arguments and attributes encoding for custom HLO enums and
// structs, so that we can pass them to custom calls.
void PopulateLmhloToXlaAttrEncoding(CustomCallAttrEncodingSet& encoding) {
  encoding
      .Add<EnumAttrEncoding<lmhlo_gpu::ActivationAttr, lmhlo_gpu::Activation,
                            se::dnn::ActivationMode>>(
          [](lmhlo_gpu::Activation value) -> se::dnn::ActivationMode {
            return ConvertConvActivationMode(value).value();
          });

#if GOOGLE_CUDA
  encoding.Add<EnumAttrEncoding<lmhlo_gpu::CublasLtMatmulEpilogueAttr,
                                lmhlo_gpu::CublasLtMatmulEpilogue,
                                se::cuda::BlasLt::Epilogue>>(
      [](lmhlo_gpu::CublasLtMatmulEpilogue value)
          -> se::cuda::BlasLt::Epilogue {
        return cublas_lt::AsBlasLtEpilogue(value).value();
      });
#endif  // GOOGLE_CUDA

  encoding
      .Add<EnumAttrEncoding<mhlo::FftTypeAttr, mhlo::FftType, se::fft::Type>>(
          [](mhlo::FftType value) -> se::fft::Type {
            switch (value) {
              case mhlo::FftType::FFT:
                return se::fft::Type::kC2CForward;
              case mhlo::FftType::IFFT:
                return se::fft::Type::kC2CInverse;
              case mhlo::FftType::RFFT:
                return se::fft::Type::kR2C;
              case mhlo::FftType::IRFFT:
                return se::fft::Type::kC2R;
              default:
                return se::fft::Type::kInvalid;
            }
          });

  using DotDimsAttr = mhlo::DotDimensionNumbersAttr;
  encoding.Add<
      xla::runtime::AggregateAttrEncoding<DotDimsAttr, DotDimensionNumbers>>(
      encoding,
      xla::runtime::AggregateAttrDef<DotDimsAttr>()
          .Add("lhs_batch", &DotDimsAttr::getLhsBatchingDimensions)
          .Add("lhs_contract", &DotDimsAttr::getLhsContractingDimensions)
          .Add("rhs_batch", &DotDimsAttr::getRhsBatchingDimensions)
          .Add("rhs_contract", &DotDimsAttr::getRhsContractingDimensions));

  using ConvDimsAttr = mhlo::ConvDimensionNumbersAttr;
  encoding.Add<
      xla::runtime::AggregateAttrEncoding<ConvDimsAttr, ConvDimensionNumbers>>(
      encoding,
      xla::runtime::AggregateAttrDef<ConvDimsAttr>()
          .Add("input_batch_dim", &ConvDimsAttr::getInputBatchDimension)
          .Add("input_feature_dim", &ConvDimsAttr::getInputFeatureDimension)
          .Add("input_spatial_dims", &ConvDimsAttr::getInputSpatialDimensions)
          .Add("kernel_in_feature_dim",
               &ConvDimsAttr::getKernelInputFeatureDimension)
          .Add("kernel_out_feature_dim",
               &ConvDimsAttr::getKernelOutputFeatureDimension)
          .Add("kernel_spatial_dims", &ConvDimsAttr::getKernelSpatialDimensions)
          .Add("output_batch_dim", &ConvDimsAttr::getOutputBatchDimension)
          .Add("output_feature_dim", &ConvDimsAttr::getOutputFeatureDimension)
          .Add("output_spatial_dims",
               &ConvDimsAttr::getOutputSpatialDimensions));

  using ConvConfigAttr = lmhlo_gpu::ConvolutionBackendConfigAttr;
  encoding.Add<
      xla::runtime::AggregateAttrEncoding<ConvConfigAttr, ConvBackendConfig>>(
      encoding,
      xla::runtime::AggregateAttrDef<ConvConfigAttr>()
          .Add("algorithm", &ConvConfigAttr::getAlgorithm)
          .Add("tensor_ops_enabled", &ConvConfigAttr::getTensorOpsEnabled)
          .Add("is_cudnn_frontend", &ConvConfigAttr::getIsCudnnFrontend)
          .Add("knob_ids", &ConvConfigAttr::getKnobIds)
          .Add("knob_values", &ConvConfigAttr::getKnobValues)
          .Add("operand_0_layout", &ConvConfigAttr::getOperand_0Layout)
          .Add("operand_1_layout", &ConvConfigAttr::getOperand_1Layout)
          .Add("result_layout", &ConvConfigAttr::getResultLayout)
          .Add("workspace_size", &ConvConfigAttr::getWorkspaceSize));
}

// -------------------------------------------------------------------------- //

template <typename MemrefArg>
static se::DeviceMemoryBase GetDeviceAddress(MemrefArg& memref) {
  uint64_t size = primitive_util::ByteWidth(memref.dtype);
  for (auto dim : memref.sizes) size *= dim;
  return se::DeviceMemoryBase(memref.data, size);
}

static se::DeviceMemoryBase GetDeviceAddress(runtime::FlatMemrefView& memref) {
  return se::DeviceMemoryBase(memref.data, memref.size_in_bytes);
}


// -------------------------------------------------------------------------- //

#if XLA_ENABLE_XCCL
FailureOr<NcclComm::Lock> GetNcclComm(const NcclExecuteParams& params,
                                      int64_t group_mode, int64_t op_id,
                                      ArrayRef<int64_t> replica_group_offsets,
                                      ArrayRef<int64_t> replica_group_values) {
  // TODO(b/233930690): Pass the attribute below as a nested array.
  // Pass an array of arrays using two vectors; one specifying all the values
  // and another specifying the (ending) offsets of each array in the other
  // vector. Example: [ [10, 20, 30, 40], [50, 60], [70, 80, 90] ] turns into
  // offsets=[4, 6, 9] values=[10, 20, 30, 40, 50, 60, 70, 80, 90].
  std::vector<ReplicaGroup> replica_groups;
  int i = 0;
  for (int64_t replica_group_end : replica_group_offsets) {
    ReplicaGroup replica_group;
    while (i < replica_group_end)
      replica_group.add_replica_ids(replica_group_values[i++]);
    replica_groups.push_back(replica_group);
  }

  auto comm =
      LockNcclComm(params, replica_groups,
                   static_cast<CollectiveOpGroupMode>(group_mode), op_id);
  if (comm.ok()) return std::move(comm.value());
  return failure();
}
#endif  // XLA_ENABLE_XCCL

FailureOr<std::vector<DeviceBufferPair>> GetDeviceBufferPairs(
    CustomCall::RemainingArgs& args) {
  // Add MemRef arguments as buffer arguments.
  const int buffer_pairs = args.size() / 2;
  std::vector<DeviceBufferPair> device_buffers;
  device_buffers.reserve(buffer_pairs);
  for (int i = 0; i < buffer_pairs; ++i) {
    auto source = args.get<runtime::StridedMemrefView>(i);
    auto destination = args.get<runtime::StridedMemrefView>(i + buffer_pairs);
    if (failed(source) || failed(destination)) {
      // Unsupported argument type.
      return failure();
    }

    int element_count = 1;
    for (int size : source->sizes) element_count *= size;
    device_buffers.emplace_back(DeviceBufferPair{
        source->dtype, element_count, GetDeviceAddress(*source),
        GetDeviceAddress(*destination)});
  }
  return device_buffers;
}

// -------------------------------------------------------------------------- //

namespace {

// TODO(ezhulenev): Today XLA represents TriangularSolve as a "classic" XLA
// custom call operation, and we provide a thin adaptor from Xla custom call
// to JitRt custom call. Once we are fully migrated to JitRt exectuion, XLA
// compiler should directly emit properly typed TriangularSolve JitRt custom
// call (no need to pass config via the serialized string).
struct TriangularSolve {
  // Adaptor from XlaCustomCall API to properly typed TriangularSolve handler.
  static absl::Status run(const ServiceExecutableRunOptions* run_options,
                          const DebugOptions* debug_options,
                          CustomCall::RemainingArgs args,
                          StringRef backend_config);

  absl::Status operator()(const ServiceExecutableRunOptions* run_options,
                          const DebugOptions* debug_options,
                          runtime::StridedMemrefView a,
                          runtime::StridedMemrefView b,
                          runtime::StridedMemrefView result,
                          runtime::FlatMemrefView temp, bool left_side,
                          bool lower, bool unit_diagonal,
                          TriangularSolveOptions::Transpose transpose_a) const;
  static TriangularSolve Handler() { return TriangularSolve(); }
};

}  // namespace

absl::Status TriangularSolve::run(
    const ServiceExecutableRunOptions* run_options,
    const DebugOptions* debug_options, CustomCall::RemainingArgs args,
    StringRef backend_config) {
  TriangularSolve handler = TriangularSolve::Handler();

  if (args.size() != 4)
    return absl::InvalidArgumentError(
        absl::StrFormat("Expected 4 arguments, got %d", args.size()));

  // Check if all arguments have the correct type.
  auto a = args.get<runtime::StridedMemrefView>(0);
  auto b = args.get<runtime::StridedMemrefView>(1);
  auto result = args.get<runtime::StridedMemrefView>(2);
  auto temp = args.get<runtime::FlatMemrefView>(3);
  if (failed(a) || failed(b) || failed(result) || failed(temp))
    return absl::InvalidArgumentError("Incorrect argument types");

  // Parse backend config string.
  TriangularSolveOptions opts;
  auto st = tsl::HumanReadableJsonToProto(backend_config.str(), &opts);
  if (!st.ok()) return ToAbslStatus(st);

  return handler(run_options, debug_options, *a, *b, *result, *temp,
                 opts.left_side(), opts.lower(), opts.unit_diagonal(),
                 opts.transpose_a());
}

absl::Status TriangularSolve::operator()(
    const ServiceExecutableRunOptions* run_options,
    const DebugOptions* debug_options, runtime::StridedMemrefView a,
    runtime::StridedMemrefView b, runtime::StridedMemrefView result,
    runtime::FlatMemrefView temp, bool left_side, bool lower,
    bool unit_diagonal, TriangularSolveOptions::Transpose transpose_a) const {
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  se::Stream* stream = run_options->stream();

  se::DeviceMemoryBase a_data = GetDeviceAddress(a);
  se::DeviceMemoryBase b_data = GetDeviceAddress(b);
  se::DeviceMemoryBase result_data = GetDeviceAddress(result);
  se::DeviceMemoryBase temp_data = GetDeviceAddress(temp);

  // Triangular solve is in-place on 'b', so copy 'b' to the output if they
  // aren't the same buffer.
  if (b.data != result.data)
    stream->ThenMemcpy(&result_data, b_data, b_data.size());

  Shape b_shape = ToShape(b);
  int64_t m = b_shape.dimensions(b_shape.rank() - 2);
  int64_t n = b_shape.dimensions(b_shape.rank() - 1);
  int64_t batch_size = std::accumulate(
      b_shape.dimensions().begin(), b_shape.dimensions().end() - 2, int64_t{1},
      [](int64_t a, int64_t b) { return a * b; });

  PrimitiveType elem_type = b.dtype;
  int64_t elem_size = ShapeUtil::ByteSizeOfPrimitiveType(elem_type);
  int64_t a_batch_stride = left_side ? m * m * elem_size : n * n * elem_size;
  int64_t b_batch_stride = m * n * elem_size;

  using Side = se::blas::Side;
  using Diagonal = se::blas::Diagonal;
  using Transpose = se::blas::Transpose;
  using UpperLower = se::blas::UpperLower;

  // Convert custom call attributes to se::blas enums.
  UpperLower uplo = lower ? UpperLower::kLower : UpperLower::kUpper;
  Side side = left_side ? Side::kLeft : Side::kRight;
  Diagonal diagonal = unit_diagonal ? Diagonal::kUnit : Diagonal::kNonUnit;

  auto transpose = [&]() -> mlir::FailureOr<Transpose> {
    switch (transpose_a) {
      case TriangularSolveOptions::NO_TRANSPOSE:
        return se::blas::Transpose::kNoTranspose;
      case TriangularSolveOptions::TRANSPOSE:
        return se::blas::Transpose::kTranspose;
      case TriangularSolveOptions::ADJOINT:
        return se::blas::Transpose::kConjugateTranspose;
      default:
        return failure();
    }
  }();

  if (failed(transpose))
    return absl::InternalError("Failed to convert transpose type");

  auto st = RunTriangulatSolve(
      a_data, result_data, temp_data, PtxOptsFromDebugOptions(*debug_options),
      uplo, side, diagonal, *transpose, elem_type, batch_size, m, n,
      a_batch_stride, b_batch_stride, stream);
  if (!st.ok()) return ToAbslStatus(st);

  return absl::OkStatus();
#else  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  return absl::InternalError("Not implemented without Gpu");
#endif
}

// -------------------------------------------------------------------------- //
// Implements JitRt custom call that forward to the Xla Custom Call handler.
//
// Longer term all Xla custom calls probably should be directly implemented as
// JitRt custom calls. However for smooth migration from Thunks to JitRt we have
// to seamlessly support all current XLA users.

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
namespace {
struct XlaCustomCall {
  using Stream = se::gpu::GpuStreamHandle;

  absl::Status operator()(const ServiceExecutableRunOptions* run_options,
                          const DebugOptions* debug_options,
                          CustomCall::RemainingArgs args,
                          StringRef call_target_name, int32_t api_version,
                          StringRef backend_config) const;
  static XlaCustomCall Handler() { return XlaCustomCall(); }
};
}  // namespace

absl::Status XlaCustomCall::operator()(
    const ServiceExecutableRunOptions* run_options,
    const DebugOptions* debug_options, CustomCall::RemainingArgs args,
    StringRef call_target_name, int32_t api_version,
    StringRef backend_config) const {
  // Pattern match custom call to a few special cases, otherwise find the custom
  // call handler regustered with the runtime.
  if (call_target_name == kTriangularSolveCallTarget)
    return TriangularSolve::run(run_options, debug_options, args,
                                backend_config);

  // Find the Xla custom call handler.
  auto& platform_name = run_options->stream()->parent()->platform()->Name();
  void* call_target = CustomCallTargetRegistry::Global()->Lookup(
      call_target_name.str(), platform_name);
  if (!call_target) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Cannot find the Xla custom call handler ", call_target_name.str()));
  }

  // Prepare pointers to buffers to pass to the Xla custom call handler.
  llvm::SmallVector<void*> buffers;
  for (unsigned i = 0; i < args.size(); ++i) {
    if (auto memref = args.get<FlatMemrefView>(i); succeeded(memref)) {
      buffers.push_back(memref->data);
      continue;
    }

    if (auto strided = args.get<StridedMemrefView>(i); succeeded(strided)) {
      int64_t size_in_bytes = primitive_util::ByteWidth(strided->dtype);
      for (int64_t size : strided->sizes) size_in_bytes *= size;
      buffers.push_back(strided->data);
      continue;
    }

    // TODO(ezhulenev): Add dialect and type to model Xla custom call holes,
    // today we rely on the fact that custom calls do not support scalar
    // arguments and we can disambiguate holes from real arguments.
    if (auto hole = args.get<int64_t>(i); succeeded(hole)) {
      buffers.push_back(nullptr);
      continue;
    }

    return absl::InvalidArgumentError(
        "Failed to get arguments as (strided) memref view");
  }

  // Original custom call API version that doesn't support returning status.
  if (api_version == CustomCallApiVersion::API_VERSION_ORIGINAL) {
    using XlaCustomCallType = void (*)(Stream, void**, const char*, size_t);
    auto xla_call_target = reinterpret_cast<XlaCustomCallType>(call_target);

    xla_call_target(se::gpu::AsGpuStreamValue(run_options->stream()),
                    buffers.data(), backend_config.data(),
                    backend_config.size());

    return absl::OkStatus();
  }

  // Xla Custom call API returning status.
  if (api_version == CustomCallApiVersion::API_VERSION_STATUS_RETURNING ||
      api_version ==
          CustomCallApiVersion::API_VERSION_STATUS_RETURNING_UNIFIED) {
    using XlaCustomCallType =
        void (*)(Stream, void**, const char*, size_t, XlaCustomCallStatus*);
    auto xla_call_target = reinterpret_cast<XlaCustomCallType>(call_target);

    XlaCustomCallStatus custom_call_status;
    xla_call_target(se::gpu::AsGpuStreamValue(run_options->stream()),
                    buffers.data(), backend_config.data(),
                    backend_config.size(), &custom_call_status);

    if (auto message = CustomCallStatusGetMessage(&custom_call_status)) {
      return absl::InternalError(message.value());
    } else {
      return absl::OkStatus();
    }
  }

  return absl::InvalidArgumentError("Incorrect custom call API version");
}

static bool CustomCall(runtime::ExecutionContext* ctx, void** args,
                       void** attrs, void** rets) {
  static auto* handler = CustomCall::Bind("xla.gpu.memcpy")
                             .UserData<const ServiceExecutableRunOptions*>()
                             .UserData<const DebugOptions*>()
                             .Arg<CustomCall::RemainingArgs>()  // args
                             .Attr<std::string_view>("call_target_name")
                             .Attr<int32_t>("api_version")
                             .Attr<std::string_view>("backend_config")
                             .To<RuntimeChecks()>(XlaCustomCall::Handler())
                             .release();

  return succeeded(Executable::Call(ctx, *handler, args, attrs, rets));
}

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

// -------------------------------------------------------------------------- //

namespace {
struct ReplicaId {
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  absl::Status operator()(const ServiceExecutableRunOptions* run_options,
                          runtime::FlatMemrefView result) const;
  static ReplicaId Handler() { return ReplicaId(); }
};
}  // namespace

absl::Status ReplicaId::operator()(
    const ServiceExecutableRunOptions* run_options,
    runtime::FlatMemrefView result) const {
  VLOG(3) << "Running ReplicaId";
  se::Stream* stream = run_options->stream();
  NcclExecuteParams params(*run_options, stream);

  StatusOr<GlobalDeviceId> global_device_id = params.GetGlobalDeviceId();
  if (!global_device_id.ok()) return ToAbslStatus(global_device_id.status());

  StatusOr<DeviceAssignment::LogicalID> logical_id =
      params.device_assn->LogicalIdForDevice(global_device_id.value());
  if (!logical_id.ok()) return ToAbslStatus(logical_id.status());

  se::DeviceMemoryBase result_data = GetDeviceAddress(result);
  params.stream->ThenMemset32(&result_data, logical_id.value().replica_id,
                              /*size=*/4);

  return absl::OkStatus();
}

static bool ReplicaId(runtime::ExecutionContext* ctx, void** args, void** attrs,
                      void** rets) {
  static auto* handler = CustomCall::Bind("xla.gpu.replica_id")
                             .UserData<const ServiceExecutableRunOptions*>()
                             .Arg<runtime::FlatMemrefView>()  // result
                             .To<RuntimeChecks()>(ReplicaId::Handler())
                             .release();

  return succeeded(Executable::Call(ctx, *handler, args, attrs, rets));
}

// -------------------------------------------------------------------------- //

namespace {
struct PartitionId {
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  absl::Status operator()(const ServiceExecutableRunOptions* run_options,
                          runtime::FlatMemrefView result) const;
  static PartitionId Handler() { return PartitionId(); }
};
}  // namespace

absl::Status PartitionId::operator()(
    const ServiceExecutableRunOptions* run_options,
    runtime::FlatMemrefView result) const {
  VLOG(3) << "Running PartitionId";
  se::Stream* stream = run_options->stream();
  NcclExecuteParams params(*run_options, stream);

  StatusOr<GlobalDeviceId> global_device_id = params.GetGlobalDeviceId();
  if (!global_device_id.ok()) return ToAbslStatus(global_device_id.status());

  StatusOr<DeviceAssignment::LogicalID> logical_id =
      params.device_assn->LogicalIdForDevice(global_device_id.value());
  if (!logical_id.ok()) return ToAbslStatus(logical_id.status());

  se::DeviceMemoryBase result_data = GetDeviceAddress(result);
  params.stream->ThenMemset32(&result_data, logical_id.value().computation_id,
                              /*size=*/4);

  return absl::OkStatus();
}

static bool PartitionId(runtime::ExecutionContext* ctx, void** args,
                        void** attrs, void** rets) {
  static auto* handler = CustomCall::Bind("xla.gpu.partition_id")
                             .UserData<const ServiceExecutableRunOptions*>()
                             .Arg<runtime::FlatMemrefView>()  // result
                             .To<RuntimeChecks()>(PartitionId::Handler())
                             .release();

  return succeeded(Executable::Call(ctx, *handler, args, attrs, rets));
}

// -------------------------------------------------------------------------- //

// Populate mapping from XLA (SE) enums/structs type id to symbol names.
void PopulateXlaGpuTypeIdNames(TypeIDNameRegistry& registry) {
#if GOOGLE_CUDA
  registry.Register<Tagged<se::cuda::BlasLt::Epilogue>>(
      "__type_id_se_cublas_lt_epilogue");
#endif  // GOOGLE_CUDA

  registry.Register<Tagged<se::dnn::ActivationMode>>(
      "__type_id_se_dnn_activation");
  registry.Register<Tagged<se::fft::Type>>("__type_id_se_fft_type");

  registry.Register<Tagged<DotDimensionNumbers>>(
      "__type_id_dot_dimension_numbers");
  registry.Register<Tagged<ConvDimensionNumbers>>(
      "__type_id_conv_dimension_numbers");
  registry.Register<Tagged<ConvBackendConfig>>("__type_id_conv_backend_config");

  RegisterTracingTypeIdNames(registry);
}

void PopulateXlaGpuCustomCalls(runtime::DirectCustomCallRegistry& registry) {
  RegisterKernelLaunchCustomCalls(registry);
  RegisterTracingCustomCalls(registry);

#if GOOGLE_CUDA
  // Graph launch kernels depend on Cuda Graph API.
  RegisterGraphLaunchCustomCalls(registry);
#endif  // GOOGLE_CUDA

  RegisterFftCustomCalls(registry);
  RegisterCholeskyCustomCalls(registry);
  RegisterCollectiveCustomCalls(registry);
  RegisterGemmCustomCalls(registry);

#if GOOGLE_CUDA
  RegisterMatmulCustomCalls(registry);
#endif  // GOOGLE_CUDA

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  registry.Register("xla.gpu.custom_call", &xla::gpu::CustomCall);
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

  RegisterConvCustomCalls(registry);
  RegisterMemcpyCustomCalls(registry);
  RegisterIoFeedCustomCalls(registry);
  RegisterMemsetCustomCalls(registry);

  // Collective operations.
  registry.Register("xla.gpu.partition_id", &xla::gpu::PartitionId);
  registry.Register("xla.gpu.replica_id", &xla::gpu::ReplicaId);
}

}  // namespace gpu
}  // namespace xla
