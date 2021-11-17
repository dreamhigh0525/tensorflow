/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu/gpu_conv_algorithm_picker.h"

#include "absl/algorithm/container.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/gpu/backend_configs.pb.h"
#include "tensorflow/compiler/xla/service/gpu/convolution_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_asm_opts_util.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_autotuning.pb.h"
#include "tensorflow/compiler/xla/service/gpu/hlo_algorithm_denylist.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"
#include "tensorflow/compiler/xla/service/gpu/stream_executor_util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/platform/logger.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/util/env_var.h"
#include "tensorflow/core/util/proto/proto_utils.h"
#include "tensorflow/stream_executor/dnn.pb.h"

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA)
#include "third_party/gpus/cudnn/cudnn.h"
#include "tensorflow/compiler/xla/service/gpu/buffer_comparator.h"
#include "tensorflow/stream_executor/gpu/redzone_allocator.h"
#endif

namespace xla {
namespace gpu {
namespace {

using absl::optional;
using se::DeviceMemoryBase;
using se::dnn::AlgorithmDesc;
using tensorflow::AutotuneResult;

class ScratchAllocator : public se::ScratchAllocator {
 public:
  ScratchAllocator(int device_ordinal,
                   se::DeviceMemoryAllocator* memory_allocator)
      : device_ordinal_(device_ordinal), memory_allocator_(memory_allocator) {}

  int64_t GetMemoryLimitInBytes() override {
    return 1LL << 32;  // 4GB.  TODO(jlebar): Tune this?
  }
  int64_t TotalAllocatedBytes() { return total_allocated_bytes_; }

  StatusOr<se::DeviceMemory<uint8>> AllocateBytes(int64_t byte_size) override;

  template <typename T>
  StatusOr<se::DeviceMemory<T>> Allocate(int64_t num_elements) {
    TF_ASSIGN_OR_RETURN(se::DeviceMemory<uint8> bytes,
                        AllocateBytes(num_elements * sizeof(T)));
    return se::DeviceMemory<T>(bytes);
  }

 private:
  const int device_ordinal_;
  se::DeviceMemoryAllocator* memory_allocator_;
  std::vector<se::OwningDeviceMemory> allocated_buffers_;
  int64_t total_allocated_bytes_ = 0;
};

StatusOr<se::DeviceMemory<uint8>> ScratchAllocator::AllocateBytes(
    int64_t byte_size) {
  CHECK_GE(byte_size, 0) << "byte_size must be positive.";
  if (byte_size > GetMemoryLimitInBytes()) {
    return se::port::Status(
        se::port::error::RESOURCE_EXHAUSTED,
        absl::StrFormat(
            "Allocating %d bytes exceeds the memory limit of %d bytes.",
            byte_size, GetMemoryLimitInBytes()));
  }

  TF_ASSIGN_OR_RETURN(se::OwningDeviceMemory allocated_buffer,
                      memory_allocator_->Allocate(device_ordinal_, byte_size,
                                                  /*retry_on_failure=*/false));
  total_allocated_bytes_ += byte_size;

  se::DeviceMemoryBase buffer_addr = *allocated_buffer;
  allocated_buffers_.push_back(std::move(allocated_buffer));
  return se::DeviceMemory<uint8>(buffer_addr);
}

StatusOr<std::vector<MaybeFusedConvRunner>> GetAlgorithms(
    const GpuConvConfig& config, se::Stream* stream, bool use_cudnn_frontend) {
  TF_ASSIGN_OR_RETURN(se::dnn::ConvolutionKind kind,
                      GetDNNConvKindFromCudnnConvKind(config.kind));

  TF_ASSIGN_OR_RETURN(se::dnn::DataType input_type,
                      GetDNNDataTypeFromPrimitiveType(config.input_type));

  TF_ASSIGN_OR_RETURN(se::dnn::DataType output_type,
                      GetDNNDataTypeFromPrimitiveType(config.output_type));

  se::StreamExecutor* stream_exec = stream->parent();

  std::vector<MaybeFusedConvRunner> result;

  switch (kind) {
    default:
      return InternalError("Unknown ConvolutionKind %d", kind);
    case se::dnn::ConvolutionKind::FORWARD_BIAS_ACTIVATION: {
      if (!config.fusion) {
        return InternalError(
            "GpuConvConfig had fusion ConvolutionKind but no FusionConfig.");
      }
      std::vector<std::unique_ptr<const se::dnn::FusedConvRunner>> runners;
      TF_RETURN_IF_ERROR(stream_exec->GetFusedConvolveRunners(
          use_cudnn_frontend,
          // This refers to the kind of convolution op inside the fusion, not
          // the whole fused graph.
          se::dnn::ConvolutionKind::FORWARD, input_type,
          BiasTypeForInputType(input_type), output_type,
          /* conv_input_scale = */ config.conv_result_scale,
          /* side_input_scale = */ config.fusion->side_input_scale, stream,
          config.input_descriptor, config.filter_descriptor,
          GetBiasDescriptor(config), config.output_descriptor, config.conv_desc,
          config.fusion->mode, &runners));
      for (auto& runner : runners) {
        TF_ASSIGN_OR_RETURN(
            auto runner_cache,
            se::dnn::LazyOpRunner<se::dnn::FusedConvOp>::FromOpRunner(
                std::move(runner)));
        result.emplace_back(std::move(runner_cache));
      }
      break;
    }

    case se::dnn::ConvolutionKind::FORWARD:
    case se::dnn::ConvolutionKind::BACKWARD_DATA:
    case se::dnn::ConvolutionKind::BACKWARD_FILTER: {
      std::vector<std::unique_ptr<const se::dnn::ConvRunner>> runners;
      // This path is cuDNN-only, where the DeviceMemoryBase arguments and the
      // allocator are unused; so, they're all provided as nullptr.
      TF_RETURN_IF_ERROR(stream_exec->GetConvolveRunners(
          use_cudnn_frontend, kind, input_type, output_type, stream,
          config.input_descriptor,
          /* input_data = */ DeviceMemoryBase(nullptr),
          config.filter_descriptor,
          /* filter_data = */ DeviceMemoryBase(nullptr),
          config.output_descriptor,
          /* output_data = */ DeviceMemoryBase(nullptr), config.conv_desc,
          nullptr, &runners));
      for (auto& runner : runners) {
        TF_ASSIGN_OR_RETURN(
            auto runner_cache,
            se::dnn::LazyOpRunner<se::dnn::ConvOp>::FromOpRunner(
                std::move(runner)));
        result.emplace_back(std::move(runner_cache));
      }
      break;
    }
  }

  return result;
}

StatusOr<std::vector<std::unique_ptr<const se::dnn::ConvRunner>>>
GetMIOpenAlgorithms(const HloCustomCallInstruction* instr,
                    absl::Span<se::DeviceMemoryBase> operand_buffers,
                    se::DeviceMemoryBase result_buffer,
                    se::StreamExecutor* stream_exec,
                    ScratchAllocator* scratch_allocator, se::Stream* stream) {
  TF_ASSIGN_OR_RETURN(GpuConvConfig config, GetGpuConvConfig(instr));

  TF_ASSIGN_OR_RETURN(se::dnn::ConvolutionKind kind,
                      GetDNNConvKindFromCudnnConvKind(config.kind));

  TF_ASSIGN_OR_RETURN(se::dnn::DataType dtype,
                      GetDNNDataTypeFromPrimitiveType(config.output_type));

  TF_ASSIGN_OR_RETURN(GpuConvParams params,
                      GetGpuConvParams(config, operand_buffers, result_buffer));

  std::vector<std::unique_ptr<const se::dnn::ConvRunner>> runners;
  TF_RETURN_IF_ERROR(stream_exec->GetConvolveRunners(
      /* use_cudnn_frontend = */ false, kind, dtype, dtype, stream,
      params.config.input_descriptor, params.input_buf,
      params.config.filter_descriptor, params.filter_buf,
      params.config.output_descriptor, params.output_buf,
      params.config.conv_desc, scratch_allocator, &runners));

  return runners;
}

string NumBytesToString(int64_t bytes) {
  return absl::StrCat(tensorflow::strings::HumanReadableNumBytes(bytes), " (",
                      bytes, "B)");
}

tensorflow::CudnnVersion GetCudnnVersion(se::StreamExecutor* stream_executor) {
  tensorflow::CudnnVersion cudnn_version;
  if (auto* dnn = stream_executor->AsDnn()) {
    StatusOr<se::dnn::VersionInfo> version_or = dnn->GetVersion();
    if (version_or.ok()) {
      const auto& version = version_or.ValueOrDie();
      cudnn_version.set_major(version.major_version());
      cudnn_version.set_minor(version.minor_version());
      cudnn_version.set_patch(version.patch());
    }
  }
  return cudnn_version;
}

tensorflow::ComputeCapability GetComputeCapability(
    se::StreamExecutor* stream_executor) {
  tensorflow::ComputeCapability cc;
  se::CudaComputeCapability se_cc =
      stream_executor->GetDeviceDescription().cuda_compute_capability();
  cc.set_major(se_cc.major);
  cc.set_minor(se_cc.minor);
  return cc;
}

void PrintPlatformInfo(const se::Stream* stream) {
  auto* se = stream->parent();
  const auto& desc = se->GetDeviceDescription();
  LOG(ERROR) << "Device: " << desc.name();
  LOG(ERROR) << "Platform: " << desc.platform_version();
  LOG(ERROR) << "Driver: " << desc.driver_version();
  LOG(ERROR) << "Runtime: " << desc.runtime_version();

  auto* dnn = se->AsDnn();
  if (dnn) {
    auto dnn_version = dnn->GetVersion();
    if (dnn_version.ok()) {
      auto v = dnn_version.ValueOrDie();
      LOG(ERROR) << "cudnn version: " << v.major_version() << "."
                 << v.minor_version() << "." << v.patch();
    }
  }
}

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA)
// Returns true if the redzones in `allocator`'s allocations are unmodified.
//
// If the redzones are modified, logs an error, sets the appropriate failure
// bits on `result`, and returns false.
//
// Returns a status if an unexpected error has occurred, and the stream
// has been poisoned.
//
// `name` is a user-friendly name for the set of redzones being checked, e.g.
// "input/output" or "scratch".
StatusOr<bool> CheckRedzones(const se::RedzoneAllocator& allocator,
                             se::Stream* stream, absl::string_view name,
                             const HloInstruction* instr,
                             AutotuneResult* result) {
  XLA_SCOPED_LOGGING_TIMER_LEVEL("CudnnConvAlgorithmPicker checking redzones",
                                 2);
  using RedzoneCheckStatus = se::RedzoneAllocator::RedzoneCheckStatus;
  TF_ASSIGN_OR_RETURN(RedzoneCheckStatus redzone_check,
                      allocator.CheckRedzones());
  if (redzone_check.ok()) {
    return true;
  }

  auto* fail = result->mutable_failure();
  fail->set_kind(AutotuneResult::REDZONE_MODIFIED);
  *fail->mutable_msg() = redzone_check.RedzoneFailureMsg();
  fail->set_buffer_address(
      reinterpret_cast<uint64_t>(redzone_check.user_buffer_address));

  LOG(ERROR) << absl::StreamFormat(
      "Detected cudnn out-of-bounds write in conv %s buffer! This is likely a "
      "cudnn bug. We will skip this algorithm in the future, but your GPU "
      "state may already be corrupted, leading to incorrect results. Within "
      "Google, no action is needed on your part. Outside of Google, please "
      "ensure you're running the latest version of cudnn. If that doesn't fix "
      "the problem, please file a bug with this full error message and we'll "
      "contact nvidia.",
      name);
  LOG(ERROR) << redzone_check.RedzoneFailureMsg();
  LOG(ERROR) << "HloInstruction " << instr->ToString();
  PrintPlatformInfo(stream);
  return false;
}
#endif

using ConvCacheKey =
    std::tuple<se::StreamExecutor*,
               /* conv->ToString(HloPrintOptions::Canonical()) */ std::string>;

struct ConvCacheStats {
  int64_t cache_hits = 0;
  int64_t cache_misses = 0;

  void LogStats() {
    VLOG(2) << "Cache hits: " << cache_hits;
    VLOG(2) << "Cache misses: " << cache_misses;
  }
};

ConvCacheKey AutotuneCacheKeyfromInstruction(
    const HloCustomCallInstruction* conv, se::StreamExecutor* se) {
  auto options = HloPrintOptions::Canonical();
  options.set_print_backend_config(true);
  return std::make_tuple(se, conv->ToString(options));
}

tensorflow::mutex autotune_cache_lock(tensorflow::LINKER_INITIALIZED);
auto& autotune_cache TF_GUARDED_BY(autotune_cache_lock) =
    *new absl::flat_hash_map<ConvCacheKey, AutotuneResult>();
auto& autotune_cache_stats TF_GUARDED_BY(autotune_cache_lock) =
    *new ConvCacheStats();
}  // anonymous namespace

StatusOr<AutotuneResult> GpuConvAlgorithmPicker::PickBestAlgorithm(
    const HloCustomCallInstruction* instr) {
  // Don't run this function concurrently on the same GPU.
  //
  // This is a bit of a hack and doesn't protect us against arbitrary concurrent
  // use of a GPU, but it's sufficient to let us compile two HLO modules
  // concurrently and then run them sequentially.
  //
  // Putting the lock in here rather than in PickBestAlgorithmNoCache lets us
  // avoid ever doing duplicate work.  If we have a cache miss, only one thread
  // will run PickBestAlgorithmImpl for a particular device.
  tensorflow::mutex_lock lock = LockGpu(stream_exec_);

  // We cache the autotuning results to avoid doing the duplicate work,
  // which can greatly improve both stability (deterministic numeric results
  // within a process for a given input) and performance (2x speedup on some
  // models).
  ConvCacheKey key = AutotuneCacheKeyfromInstruction(instr, stream_exec_);
  {
    tensorflow::mutex_lock lock(autotune_cache_lock);
    auto it = autotune_cache.find(key);
    if (it != autotune_cache.end()) {
      autotune_cache_stats.cache_hits++;
      return it->second;
    }
    autotune_cache_stats.cache_misses++;
  }

  // Make sure any previous activity on this executor is done. We don't want
  // other work still running on the GPU to interfere with autotuning.
  if (!stream_exec_->SynchronizeAllActivity()) {
    return InternalError(
        "Failed to synchronize GPU for autotuning conv instruction: %s",
        std::get<1>(key) /* instr */);
  }

  // allocator either points to this->allocator_ or, if that's null, to a
  // se::StreamExecutorMemoryAllocator for stream_exec_.
  se::DeviceMemoryAllocator* allocator;
  optional<se::StreamExecutorMemoryAllocator> se_allocator;
  if (allocator_ != nullptr) {
    allocator = allocator_;
  } else {
    se_allocator.emplace(stream_exec_);
    allocator = &*se_allocator;
  }

  TF_ASSIGN_OR_RETURN(se::Stream* const stream,
                      allocator->GetStream(stream_exec_->device_ordinal()));
  StatusOr<AutotuneResult> result_or(InternalError("Unknown platform."));
  // Check StreamExecutor on which platform it is. ROCm and Cuda implementation
  // have diverged. Specifically, we need to make sure redzone allocator related
  // utilities are not used in ROCm routine
  if (stream_exec_->platform_kind() == se::PlatformKind::kROCm) {
    result_or = PickBestAlgorithmNoCacheRocm(instr, allocator, stream);
  } else if (stream_exec_->platform_kind() == se::PlatformKind::kCuda) {
#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA)
    result_or = PickBestAlgorithmNoCacheCuda(instr, allocator, stream);
#endif
  }

  if (result_or.ok()) {
    tensorflow::mutex_lock lock(autotune_cache_lock);
    CHECK(autotune_cache.insert({key, result_or.ValueOrDie()}).second);
  }
  return result_or;
}

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA)
StatusOr<tensorflow::AutotuneResult>
GpuConvAlgorithmPicker::PickBestAlgorithmNoCacheCuda(
    const HloCustomCallInstruction* instr, se::DeviceMemoryAllocator* allocator,
    se::Stream* stream) {
  // Right now Redzone allocator is available in Cuda target only
  XLA_SCOPED_LOGGING_TIMER(absl::StrCat(
      "GpuConvAlgorithmPicker::PickBestAlgorithmImpl for ", instr->ToString()));

  const Shape& result_shape = instr->shape().tuple_shapes(0);
  int64_t rng_state = 0;

  const HloModuleConfig& hlo_module_config = instr->GetModule()->config();
  const int32_t conv_autotune_level =
      hlo_module_config.debug_options().xla_gpu_autotune_level();
  const bool init_conv_data = conv_autotune_level >= 2;
  const bool check_conv = conv_autotune_level >= 4;
  const auto initialize_buffer = [init_conv_data, &stream, &rng_state](
                                     DeviceMemoryBase buffer,
                                     const Shape& buffer_shape) {
    if (init_conv_data) {
      InitializeBuffer(stream, buffer_shape.element_type(), &rng_state, buffer);
    }
  };

  // Allocate space for the input, filter, and output of the convolution.
  const int64_t redzone_size =
      check_conv ? se::RedzoneAllocator::kDefaultRedzoneSize : 0;
  se::RedzoneAllocator input_output_allocator(
      stream, allocator,
      PtxOptsFromDebugOptions(hlo_module_config.debug_options()),
      /*memory_limit=*/se::RedzoneAllocator::kDefaultMemoryLimit,
      /*redzone_size=*/redzone_size);
  std::vector<se::DeviceMemoryBase> operand_buffers;
  for (const auto* operand : instr->operands()) {
    TF_ASSIGN_OR_RETURN(auto buffer,
                        input_output_allocator.AllocateBytes(
                            ShapeUtil::ByteSizeOf(operand->shape())));
    initialize_buffer(buffer, operand->shape());
    operand_buffers.push_back(buffer);
  }
  TF_ASSIGN_OR_RETURN(auto result_buffer,
                      input_output_allocator.AllocateBytes(
                          ShapeUtil::ByteSizeOf(result_shape)));
  initialize_buffer(result_buffer, result_shape);

  TF_ASSIGN_OR_RETURN(auto backend_config,
                      instr->backend_config<CudnnConvBackendConfig>());

  optional<BufferComparator> comparator;
  // Use the first algorithm that's supported as reference. There isn't a
  // particular reason to use it, as any algorithm suffices. It doesn't make
  // this algorithm considered correct, though.
  se::DeviceMemoryBase reference_result_buffer;
  AlgorithmDesc first_algorithm;

  TF_ASSIGN_OR_RETURN(CudnnConvKind kind, GetCudnnConvKind(instr));
  std::vector<AutotuneResult> profile_results;

  const DebugOptions& debug_options =
      instr->GetModule()->config().debug_options();

  const bool crash_on_checking_failure =
      debug_options.xla_gpu_crash_on_verification_failures();

  std::string canonical_hlo =
      std::get<1>(AutotuneCacheKeyfromInstruction(instr, stream_exec_));

  string blas_version;
  if (auto* blas = stream_exec_->AsBlas()) {
    (void)blas->GetVersion(&blas_version);
  }

  absl::Span<const AlgorithmDesc> disabled_algos = GetDisabledConvAlgorithms(
      GetComputeCapability(stream_exec_), GetCudnnVersion(stream_exec_),
      blas_version, canonical_hlo);

  TF_ASSIGN_OR_RETURN(GpuConvConfig config, GetGpuConvConfig(instr));

  const bool cudnn_frontend_enabled = instr->parent()
                                          ->parent()
                                          ->config()
                                          .debug_options()
                                          .xla_gpu_enable_cudnn_frontend();
  // Fused convolutions with identity activations are broken in that they
  // implicitly do ReLU on some engines, and we can't reliably detect which
  // ones.  TODO(awpr): this may be fixed in 8.2.5; consider letting this case
  // through for newer versions.
  const bool is_broken_identity_fused_conv =
      config.fusion && config.fusion->mode == se::dnn::ActivationMode::kNone;
  // All current versions of the frontend API lack support for int8x32
  // convolutions.
  const bool is_unsupported_int8x32 =
      config.input_type == S8 &&
      config.input_descriptor.layout() == se::dnn::kBatchDepthYX32;
  const bool use_cudnn_frontend = cudnn_frontend_enabled &&
                                  !is_broken_identity_fused_conv &&
                                  !is_unsupported_int8x32;

  TF_ASSIGN_OR_RETURN(std::vector<MaybeFusedConvRunner> runners,
                      GetAlgorithms(config, stream, use_cudnn_frontend));

  for (auto& runner_cache : runners) {
    auto alg = runner_cache.ToAlgorithmDesc();

    XLA_SCOPED_LOGGING_TIMER_LEVEL(
        absl::StrCat("CudnnConvAlgorithmPicker::PickBestAlgorithm algo ",
                     alg.ToString()),
        2);

    profile_results.emplace_back();
    AutotuneResult& result = profile_results.back();
    *result.mutable_algorithm() = alg.ToProto();

    auto set_failure = [&result](AutotuneResult::FailureKind kind,
                                 absl::string_view msg) {
      result.mutable_failure()->set_kind(kind);
      result.mutable_failure()->set_msg(/* *sigh* */ msg.data(), msg.size());
    };

    AlgorithmDesc alg_key(alg.algo_id(), alg.tensor_ops_enabled(),
                          absl::nullopt);

    if (absl::c_linear_search(disabled_algos, alg_key)) {
      LOG(INFO) << "Omitted potentially buggy algorithm " << alg.ToString()
                << " for conv " << instr->ToString();
      set_failure(AutotuneResult::DISQUALIFIED,
                  "Disqualified for being known-buggy.");
      continue;
    }

    // For fused convolutions with the identity function as the activation, only
    // ALGO_IMPLICIT_PRECOMP_GEMM does the right thing. Other algorithms
    // silently do Relu. See
    // https://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#cudnnConvolutionBiasActivationForward
    if (kind == CudnnConvKind::kForwardActivation &&
        backend_config.activation_mode() == se::dnn::ActivationMode::kNone &&
        alg.algo_id() != CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM) {
      set_failure(AutotuneResult::DISQUALIFIED,
                  "Disqualified for implicit RELU.");
      continue;
    }

    se::RedzoneAllocator scratch_allocator(
        stream, allocator,
        PtxOptsFromDebugOptions(hlo_module_config.debug_options()));
    se::dnn::ProfileResult profile_result;
    VLOG(3) << "Trying algorithm " << alg.ToString() << " for "
            << instr->ToString();

    absl::optional<size_t> workspace_size =
        runner_cache.ToAlgorithmDesc().workspace_size();
    if (!workspace_size) {
      set_failure(AutotuneResult::UNKNOWN,
                  "Internal error: missing workspace size from "
                  "OpRunner::ToAlgorithmDesc()");
      continue;
    }
    auto scratch_or = scratch_allocator.AllocateBytes(*workspace_size);
    if (!scratch_or.ok()) {
      set_failure(AutotuneResult::DISQUALIFIED,
                  absl::StrCat("Scratch allocation failed: ",
                               scratch_or.status().ToString()));
      continue;
    }
    se::DeviceMemoryBase scratch_memory = scratch_or.ValueOrDie();

    // Use assignment instead of brace-list to make GCC 4.9 happy.
    RunConvOptions options;
    options.profile_result = &profile_result;
    options.runner_cache = &runner_cache;
    Status launch_status =
        RunGpuConv(config, absl::MakeSpan(operand_buffers), result_buffer,
                   scratch_memory, stream, options);

    if (!launch_status.ok()) {
      VLOG(4) << "Launch failed: " << launch_status;
      set_failure(AutotuneResult::DISQUALIFIED,
                  absl::StrCat("Profiling failure on cuDNN engine ",
                               alg.ToString(), ": ", launch_status.ToString()));
      continue;
    }

    if (!profile_result.is_valid()) {
      VLOG(4) << "Launch succeeded but profile result is invalid.";
      // Not DISQUALIFIED: this means something went wrong internally.
      set_failure(
          AutotuneResult::UNKNOWN,
          absl::StrCat("Launch succeeded but profile result is invalid, "
                       "with cuDNN engine ",
                       alg.ToString(), ": ", launch_status.ToString()));
      continue;
    }

    int64_t scratch_bytes_used =
        scratch_allocator.TotalAllocatedBytesExcludingRedzones();
    result.set_scratch_bytes(scratch_bytes_used);
    *result.mutable_run_time() = tensorflow::proto_utils::ToDurationProto(
        absl::Milliseconds(profile_result.elapsed_time_in_ms()));

    if (!check_conv) {
      continue;
    }

    // Check for writes to redzones.
    TF_ASSIGN_OR_RETURN(bool input_output_allocator_redzone_clear,
                        CheckRedzones(input_output_allocator, stream,
                                      "input/output", instr, &result));

    TF_ASSIGN_OR_RETURN(
        bool scratch_allocator_redzone_clear,
        CheckRedzones(scratch_allocator, stream, "scratch", instr, &result));

    if (!input_output_allocator_redzone_clear ||
        !scratch_allocator_redzone_clear) {
      AlgorithmDenylist proto;
      auto entry = proto.add_entries();
      entry->set_hlo(canonical_hlo);
      *entry->mutable_cc() = GetComputeCapability(stream_exec_);
      *entry->mutable_cudnn_version() = GetCudnnVersion(stream_exec_);
      entry->set_blas_version(blas_version);
      auto algo = entry->add_algos();
      algo->set_id(alg.algo_id());
      algo->set_tensor_ops(alg.tensor_ops_enabled());

      LOG(ERROR) << "To denylist this algorithm for this convolution, "
                    "copy-paste the following "
                    "proto to the denylist file pointed by XLA_FLAGS "
                    "--xla_gpu_algorithm_denylist_path="
                 << GetDebugOptionsFromFlags().xla_gpu_algorithm_denylist_path()
                 << " : " << proto.ShortDebugString();
      continue;
    }

    if (comparator.has_value()) {
      XLA_SCOPED_LOGGING_TIMER_LEVEL("BufferComparator::CompareEqual", 2);
      StatusOr<bool> compare_result = comparator->CompareEqual(
          stream, reference_result_buffer, result_buffer);
      if (!compare_result.ok()) {
        LOG(ERROR) << "Unable to compare " << first_algorithm.ToString()
                   << " against " << alg.ToString() << " for "
                   << instr->ToString() << ": " << compare_result.status();
        if (compare_result.status().code() ==
            tensorflow::error::RESOURCE_EXHAUSTED) {
          // Possibly OOM. Propagate the error.
          return compare_result.status();
        }
        CHECK(!crash_on_checking_failure);
      } else if (!compare_result.ValueOrDie()) {
        LOG(ERROR)
            << "Results mismatch between different convolution algorithms. "
               "This is likely a bug/unexpected loss of precision in cudnn.\n"
            << instr->ToString() << " for " << first_algorithm.ToString()
            << " vs " << alg.ToString();
        PrintPlatformInfo(stream);
        VLOG(1) << "Full module on failure: \n"
                << instr->GetModule()->ToString();
        auto* fail = result.mutable_failure();
        fail->set_kind(AutotuneResult::WRONG_RESULT);
        fail->set_buffer_address(
            reinterpret_cast<uint64_t>(result_buffer.opaque()));
        auto* reference_conv = fail->mutable_reference_conv();
        reference_conv->set_algorithm(first_algorithm.algo_id());
        reference_conv->set_tensor_ops_enabled(
            first_algorithm.tensor_ops_enabled());
      }
    } else {
      XLA_SCOPED_LOGGING_TIMER_LEVEL("BufferComparator::Create", 2);
      comparator.emplace(result_shape, hlo_module_config);
      TF_ASSIGN_OR_RETURN(
          reference_result_buffer,
          input_output_allocator.AllocateBytes(result_buffer.size()));
      stream->ThenMemcpy(&reference_result_buffer, result_buffer,
                         result_buffer.size());
      first_algorithm = alg;
    }
  }

  // Log the autotuning result.
  {
    tensorflow::AutotuningLog log;
    {
      ConvInstructionLog instr_log;
      *instr_log.mutable_instruction() = instr->ToProto();
      for (int i = 0; i < instr->operand_count(); i++) {
        *instr_log.add_operand_shapes() = instr->operand(i)->shape().ToProto();
        instr_log.add_operand_addresses(
            reinterpret_cast<uint64_t>(operand_buffers[i].opaque()));
      }
      instr_log.set_result_address(
          reinterpret_cast<uint64_t>(result_buffer.opaque()));
      log.mutable_instr()->PackFrom(instr_log);
    }
    for (const auto& profile : profile_results) {
      *log.add_results() = profile;
    }
    *log.mutable_compute_capability() = GetComputeCapability(stream_exec_);
    *log.mutable_cudnn_version() = GetCudnnVersion(stream_exec_);
    log.set_device_pci_bus_id(
        stream_exec_->GetDeviceDescription().pci_bus_id());
    log.set_blas_version(blas_version);
    VLOG(1) << "Autotuning result: " << log.ShortDebugString();
    // If we crash on checking failure, we are in a testing/benchmark mode, thus
    // omitting logging through the logger.
    if (!crash_on_checking_failure) {
      tensorflow::Logger::GetSingleton()->LogProto(log);
    } else {
      // Crash on miscompares and redzone violations if desired.
      for (const auto& profile : profile_results) {
        if (profile.has_failure() &&
            profile.failure().kind() != AutotuneResult::DISQUALIFIED) {
          LOG(FATAL) << "crash_on_checking_failure encountered errors:\n\n"
                     << log.DebugString();
        }
      }
    }
  }

  TF_ASSIGN_OR_RETURN(AutotuneResult selected_algorithm,
                      PickBestResult(profile_results, *instr));
  return selected_algorithm;
}
#endif

StatusOr<tensorflow::AutotuneResult>
GpuConvAlgorithmPicker::PickBestAlgorithmNoCacheRocm(
    const HloCustomCallInstruction* instr, se::DeviceMemoryAllocator* allocator,
    se::Stream* stream) {
  XLA_SCOPED_LOGGING_TIMER(absl::StrCat(
      "GpuConvAlgorithmPicker::PickBestAlgorithmImpl for ", instr->ToString()));

  const auto device_ordinal = stream_exec_->device_ordinal();
  std::vector<se::DeviceMemoryBase> operand_buffers;

  ScratchAllocator input_output_allocator(device_ordinal, allocator);
  const auto initialize_buffer = [stream](DeviceMemoryBase buffer) {
    // Although we don't have evidence this matters, zero out the buffers
    // before autotuning.  It's conceivable that using uninitialized memory as
    // the inputs might affect performance if e.g. the inputs contain
    // denormals, and this is easy enough.
    stream->ThenMemZero(&buffer, buffer.size());
  };

  // Allocate space for the input, filter, and output of the convolution.  We
  // use a ScratchAllocator for this instead of calling allocator_ directly so
  // that our allocations don't leak.
  for (const auto* operand : instr->operands()) {
    TF_ASSIGN_OR_RETURN(auto buffer,
                        input_output_allocator.AllocateBytes(
                            ShapeUtil::ByteSizeOf(operand->shape())));
    initialize_buffer(buffer);
    operand_buffers.push_back(buffer);
  }

  TF_ASSIGN_OR_RETURN(
      auto result_buffer,
      input_output_allocator.AllocateBytes(
          ShapeUtil::ByteSizeOf(instr->shape().tuple_shapes(0))));
  initialize_buffer(result_buffer);

  ScratchAllocator scratch_allocator(device_ordinal, allocator);

  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<const se::dnn::ConvRunner>> runners,
      GetMIOpenAlgorithms(instr, absl::MakeSpan(operand_buffers), result_buffer,
                          stream_exec_, &scratch_allocator, stream));

  std::vector<AutotuneResult> profile_results;

  if (runners.size() == 1) {
    TF_ASSIGN_OR_RETURN(auto alg, runners[0]->ToAlgorithmDesc());
    auto algorithm_proto = alg.ToProto();
    profile_results.emplace_back();
    auto& result = profile_results.back();
    *result.mutable_algorithm() = algorithm_proto;

    result.set_scratch_bytes(runners[0]->GetWorkspaceSize());

    // TODO(awpr): if the profile result time for a singleton algorithm is
    // needed, plumb it via OpRunner; we'll need to do this to let TF ops avoid
    // re-profiling ROCm algorithms anyway.
    *result.mutable_run_time() =
        tensorflow::proto_utils::ToDurationProto(absl::Milliseconds(-1));
  } else {
    TF_ASSIGN_OR_RETURN(GpuConvConfig config, GetGpuConvConfig(instr));
    for (auto& runner : runners) {
      TF_ASSIGN_OR_RETURN(auto alg, runner->ToAlgorithmDesc());
      XLA_SCOPED_LOGGING_TIMER_LEVEL(
          absl::StrCat("CudnnConvAlgorithmPicker::PickBestAlgorithm algo ",
                       alg.ToString()),
          2);

      se::dnn::ProfileResult profile_result;
      VLOG(3) << "Trying algorithm " << alg.ToString() << " for "
              << instr->ToString();

      TF_ASSIGN_OR_RETURN(
          DeviceMemoryBase scratch_memory,
          scratch_allocator.AllocateBytes(runner->GetWorkspaceSize()));

      TF_ASSIGN_OR_RETURN(auto lazy_runner,
                          se::dnn::LazyOpRunner<se::dnn::ConvOp>::FromOpRunner(
                              std::move(runner)));

      MaybeFusedConvRunner runner_cache(std::move(lazy_runner));

      // Use assignment instead of brace-list to make GCC 4.9 happy.
      RunConvOptions options;
      options.profile_result = &profile_result;
      options.runner_cache = &runner_cache;
      Status launch_status =
          RunGpuConv(config, absl::MakeSpan(operand_buffers), result_buffer,
                     scratch_memory, stream, options);

      if (!launch_status.ok()) {
        continue;
      }

      if (!profile_result.is_valid()) {
        continue;
      }

      profile_results.emplace_back();
      AutotuneResult& result = profile_results.back();
      *result.mutable_algorithm() = alg.ToProto();

      int64_t scratch_bytes_used = scratch_allocator.TotalAllocatedBytes();
      result.set_scratch_bytes(scratch_bytes_used);
      *result.mutable_run_time() = tensorflow::proto_utils::ToDurationProto(
          absl::Milliseconds(profile_result.elapsed_time_in_ms()));
    }
  }

  TF_ASSIGN_OR_RETURN(AutotuneResult selected_algorithm,
                      PickBestResult(profile_results, *instr));
  return selected_algorithm;
}

StatusOr<bool> GpuConvAlgorithmPicker::RunOnInstruction(HloInstruction* instr) {
  CHECK(IsCustomCallToDnnConvolution(*instr));

  const bool strict = instr->parent()
                          ->parent()
                          ->config()
                          .debug_options()
                          .xla_gpu_strict_conv_algorithm_picker();

  StatusOr<AutotuneResult> best_algo_or =
      PickBestAlgorithm(Cast<HloCustomCallInstruction>(instr));
  if (!best_algo_or.ok()) {
    auto msg = absl::StrFormat(
        "Failed to determine best cudnn convolution algorithm for:\n%s\n\n"
        "Original error: %s",
        instr->ToString(), best_algo_or.status().ToString());

    if (strict) {
      return Unknown(
          "%s\n\nTo ignore this failure and try to use a fallback algorithm "
          "(which may have suboptimal performance), use "
          "XLA_FLAGS=--xla_gpu_strict_conv_algorithm_picker=false.  Please "
          "also file a bug for the root cause of failing autotuning.",
          msg);
    }
    LOG(WARNING)
        << msg << "\n\nAs a result, convolution performance may be suboptimal.";
    return false;
  }

  auto best_algo = std::move(best_algo_or).ValueOrDie();
  VLOG(2) << "Setting cudnn conv to use algorithm "
          << best_algo.conv().algorithm() << " and "
          << NumBytesToString(best_algo.scratch_bytes())
          << " of scratch memory: " << instr->ToString()
          << " tensor_ops_enabled: " << best_algo.conv().tensor_ops_enabled();

  // Replace instr with a new CustomCall which has the correct algorithm, and
  // whose output shape has the appropriate amount of scratch memory.
  HloComputation* computation = instr->parent();
  Shape new_call_shape = ShapeUtil::MakeTupleShape(
      {instr->shape().tuple_shapes(0),
       ShapeUtil::MakeShape(U8, {best_algo.scratch_bytes()})});

  TF_ASSIGN_OR_RETURN(CudnnConvBackendConfig backend_config,
                      instr->backend_config<CudnnConvBackendConfig>());
  *backend_config.mutable_algorithm() = best_algo.algorithm();
  backend_config.mutable_algorithm()->mutable_workspace_size()->set_value(
      best_algo.scratch_bytes());

  HloInstruction* new_call = computation->AddInstruction(
      instr->CloneWithNewOperands(new_call_shape, instr->operands()));

  VLOG(2) << "Replacing convolution " << instr->ToString() << " with "
          << new_call->ToString();

  TF_RETURN_IF_ERROR(new_call->set_backend_config(backend_config));

  // Repackage new_call so it has the same shape as the original call, namely
  // (conv_result, u8[0]).
  HloInstruction* new_tuple =
      computation->AddInstruction(HloInstruction::CreateTuple(
          {computation->AddInstruction(HloInstruction::CreateGetTupleElement(
               new_call_shape.tuple_shapes(0), new_call, 0)),
           computation->AddInstruction(HloInstruction::CreateConstant(
               LiteralUtil::CreateR1<uint8>({})))}));

  TF_RETURN_IF_ERROR(instr->parent()->ReplaceInstruction(instr, new_tuple));
  return true;
}

StatusOr<bool> GpuConvAlgorithmPicker::RunOnComputation(
    HloComputation* computation) {
  std::vector<HloInstruction*> convs;
  for (auto* instr : computation->instructions()) {
    if (IsCustomCallToDnnConvolution(*instr)) {
      convs.push_back(instr);
    }
  }

  bool changed = false;
  for (auto* instr : convs) {
    TF_ASSIGN_OR_RETURN(bool result, RunOnInstruction(instr));
    changed |= result;
  }
  return changed;
}

StatusOr<bool> GpuConvAlgorithmPicker::Run(HloModule* module) {
  XLA_SCOPED_LOGGING_TIMER("GpuConvAlgorithmPicker");

  if (module->config().debug_options().xla_gpu_autotune_level() == 0) {
    VLOG(2) << "Convolution auto-tuning disabled, GpuConvAlgorithmPicker "
               "returning early.";
    return false;
  }

  bool changed = false;
  for (HloComputation* computation : module->MakeNonfusionComputations()) {
    TF_ASSIGN_OR_RETURN(bool result, RunOnComputation(computation));
    changed |= result;
  }

  {
    tensorflow::mutex_lock lock(autotune_cache_lock);
    autotune_cache_stats.LogStats();
  }

  return changed;
}

}  // namespace gpu
}  // namespace xla
