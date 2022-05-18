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
#include <utility>

#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/xla/service/gpu/gemm_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/matmul_utils.h"
#include "tensorflow/compiler/xla/service/gpu/stream_executor_util.h"
#include "tensorflow/compiler/xla/service/service_executable_run_options.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tfrt/jitrt/custom_call.h"  // from @tf_runtime
#include "tfrt/jitrt/jitrt.h"  // from @tf_runtime
#include "tfrt/dtype/dtype.h"  // from @tf_runtime

TFRT_DEFINE_EXPLICIT_DENSE_TYPE_ID(tfrt::jitrt::CustomCall,
                                   xla::gpu::JitRtKernelsCache);
TFRT_DEFINE_EXPLICIT_DENSE_TYPE_ID(tfrt::jitrt::CustomCall,
                                   xla::gpu::JitRtGemmConfigCache);
TFRT_DEFINE_EXPLICIT_DENSE_TYPE_ID(tfrt::jitrt::CustomCall,
                                   const xla::ServiceExecutableRunOptions);
TFRT_DEFINE_EXPLICIT_DENSE_TYPE_ID(tfrt::jitrt::CustomCall,
                                   const xla::DebugOptions);

namespace xla {
namespace gpu {

using llvm::ArrayRef;
using llvm::orc::MangleAndInterner;
using llvm::orc::SymbolMap;

using mlir::failure;
using mlir::LogicalResult;
using mlir::StringRef;
using mlir::succeeded;
using mlir::success;

using tfrt::jitrt::CustomCall;
using tfrt::jitrt::Executable;

namespace se = ::stream_executor;
namespace jitrt = ::tfrt::jitrt;
namespace runtime = ::tfrt::jitrt::runtime;

// Disable all CustomCall checks in optimized build.
static constexpr CustomCall::RuntimeChecks RuntimeChecks() {
#if defined(NDEBUG)
  return CustomCall::RuntimeChecks::kNone;
#else
  return CustomCall::RuntimeChecks::kDefault;
#endif
}

// -------------------------------------------------------------------------- //

se::KernelBase* JitRtKernelsCache::Get(se::StreamExecutor* executor,
                                       const char* data) {
  Key key = {executor, data};

  absl::MutexLock lock(&mutex_);
  auto it = kernels_cache_.find(key);
  if (it != kernels_cache_.end()) return it->second.get();

  return nullptr;
}

se::KernelBase* JitRtKernelsCache::Set(se::StreamExecutor* executor,
                                       const char* data,
                                       std::unique_ptr<se::KernelBase> kernel) {
  Key key = {executor, data};

  absl::MutexLock lock(&mutex_);
  auto it = kernels_cache_.find(key);
  if (it != kernels_cache_.end()) return it->second.get();

  auto emplaced = kernels_cache_.try_emplace(key, std::move(kernel));
  return emplaced.first->second.get();
}

static se::DeviceMemoryBase GetDeviceAddress(jitrt::MemrefView& memref) {
  uint64_t size = tfrt::GetHostSize(memref.dtype);
  for (auto dim : memref.sizes) size *= dim;
  return se::DeviceMemoryBase(memref.data, size);
}

static se::DeviceMemoryBase GetDeviceAddress(jitrt::FlatMemrefView& memref) {
  return se::DeviceMemoryBase(memref.data, memref.size_in_bytes);
}

// -------------------------------------------------------------------------- //

const GemmConfig* JitRtGemmConfigCache::Get(int64_t uid) {
  absl::MutexLock lock(&mutex_);
  auto it = configs_.find(uid);
  if (it != configs_.end()) return &it->second;
  return nullptr;
}

const GemmConfig* JitRtGemmConfigCache::Set(int64_t uid, GemmConfig config) {
  absl::MutexLock lock(&mutex_);
  auto it = configs_.find(uid);
  if (it != configs_.end()) return &it->second;

  auto emplaced = configs_.try_emplace(uid, std::move(config));
  return &emplaced.first->second;
}

// -------------------------------------------------------------------------- //

static PrimitiveType ToPrimitiveType(tfrt::DType dtype) {
  switch (dtype) {
    case tfrt::DType::F32:
      return PrimitiveType::F32;
    case tfrt::DType::F64:
      return PrimitiveType::F64;
    default:
      LOG(FATAL) << "Unsupported data type: " << dtype;
  }
}

static Shape ToShape(const jitrt::MemrefView& memref) {
  PrimitiveType type = ToPrimitiveType(memref.dtype);
  return ShapeUtil::MakeShape(type, memref.sizes);
}

static StatusOr<GemmConfig> GetGemmConfig(
    const DebugOptions* debug_options, const jitrt::MemrefView& lhs,
    const jitrt::MemrefView& rhs, const jitrt::MemrefView& out,
    int64_t algorithm, double alpha_imag, double alpha_real,
    ArrayRef<int64_t> lhs_batch, ArrayRef<int64_t> lhs_contract,
    ArrayRef<int64_t> rhs_batch, ArrayRef<int64_t> rhs_contract,
    llvm::Optional<double> beta = llvm::None) {
  return GemmConfig::For(ToShape(lhs), lhs_batch, lhs_contract, ToShape(rhs),
                         rhs_batch, rhs_contract, ToShape(out), alpha_real,
                         alpha_imag, beta.getValueOr(0.0), algorithm,
                         debug_options->xla_gpu_enable_cublaslt());
}

// -------------------------------------------------------------------------- //

namespace {
struct LaunchFunc {
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  LogicalResult operator()(const ServiceExecutableRunOptions* run_options,
                           JitRtKernelsCache* kernels_cache,
                           int32_t grid_size_x, int32_t grid_size_y,
                           int32_t grid_size_z, int32_t block_size_x,
                           int32_t block_size_y, int32_t block_size_z,
                           CustomCall::RemainingArgs args, StringRef ptx,
                           StringRef name) const;

  static LaunchFunc Handler() { return LaunchFunc(); }
};
}  // namespace

LogicalResult LaunchFunc::operator()(
    const ServiceExecutableRunOptions* run_options,
    JitRtKernelsCache* kernels_cache, int32_t grid_size_x, int32_t grid_size_y,
    int32_t grid_size_z, int32_t block_size_x, int32_t block_size_y,
    int32_t block_size_z, CustomCall::RemainingArgs args, StringRef ptx,
    StringRef name) const {
  se::Stream* stream = run_options->stream();
  se::StreamExecutor* executor = stream->parent();

  LaunchDimensions launch_dimensions(
      {grid_size_x, grid_size_y, grid_size_z},
      {block_size_x, block_size_y, block_size_z});

  se::KernelBase* kernel = kernels_cache->Get(executor, ptx.data());

  // If kernel does not exists create it from the ptx.
  if (kernel == nullptr) {
    auto created = CreateKernel(absl::string_view(name.data(), name.size()),
                                args.size(), ptx.data(), {}, executor);
    if (!created.ok()) return failure();

    kernel = kernels_cache->Set(executor, ptx.data(), std::move(*created));
  }

  VLOG(3) << "Launching " << kernel->name();
  absl::InlinedVector<se::DeviceMemoryBase, 4> buffer_args;
  buffer_args.reserve(args.size());

  // Add MemRef arguments as buffer arguments.
  for (unsigned i = 0; i < args.size(); ++i) {
    auto memref = args.get<jitrt::FlatMemrefView>(i);
    if (failed(memref)) return failure();
    buffer_args.emplace_back(GetDeviceAddress(*memref));
  }

  // Execute device kernel on a main stream.
  auto executed =
      ExecuteKernelOnStream(*kernel, buffer_args, launch_dimensions, stream);
  if (!executed.ok()) return failure();

  return success();
}

static bool LaunchFunc(runtime::KernelContext* ctx, void** args, void** attrs) {
  static auto* handler = CustomCall::Bind("xla.gpu.func.launch")
                             .UserData<const ServiceExecutableRunOptions*>()
                             .UserData<JitRtKernelsCache*>()
                             .Arg<int32_t>()   // grid_size_x
                             .Arg<int32_t>()   // grid_size_y
                             .Arg<int32_t>()   // grid_size_z
                             .Arg<int32_t>()   // block_size_x
                             .Arg<int32_t>()   // block_size_y
                             .Arg<int32_t>()   // block_size_x
                             .RemainingArgs()  // args
                             .Attr<StringRef>("ptx")
                             .Attr<StringRef>("kernel")
                             .To<RuntimeChecks()>(LaunchFunc::Handler())
                             .release();

  return succeeded(handler->call(args, attrs, Executable::GetUserData(ctx)));
}

// -------------------------------------------------------------------------- //

namespace {
struct Gemm {
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  LogicalResult operator()(const ServiceExecutableRunOptions* run_options,
                           const DebugOptions* debug_options,
                           JitRtGemmConfigCache* configs, jitrt::MemrefView lhs,
                           jitrt::MemrefView rhs, jitrt::MemrefView out,
                           int64_t algorithm, double alpha_imag,
                           double alpha_real, ArrayRef<int64_t> lhs_batch,
                           ArrayRef<int64_t> lhs_contract,
                           ArrayRef<int64_t> rhs_batch,
                           ArrayRef<int64_t> rhs_contract, int64_t uid) const;

  static Gemm Handler() { return Gemm(); }
};
}  // namespace

LogicalResult Gemm::operator()(
    const ServiceExecutableRunOptions* run_options,
    const DebugOptions* debug_options, JitRtGemmConfigCache* configs,
    jitrt::MemrefView lhs, jitrt::MemrefView rhs, jitrt::MemrefView out,
    int64_t algorithm, double alpha_imag, double alpha_real,
    ArrayRef<int64_t> lhs_batch, ArrayRef<int64_t> lhs_contract,
    ArrayRef<int64_t> rhs_batch, ArrayRef<int64_t> rhs_contract,
    int64_t uid) const {
  se::DeviceMemoryBase lhs_data = GetDeviceAddress(lhs);
  se::DeviceMemoryBase rhs_data = GetDeviceAddress(rhs);
  se::DeviceMemoryBase output_data = GetDeviceAddress(out);

  se::OwningScratchAllocator<> scratch_allocator(run_options->device_ordinal(),
                                                 run_options->allocator());

  VLOG(3) << "Running GEMM";
  se::Stream* stream = run_options->stream();

  // Find the gemm config for this instance of operation based on uid.
  const GemmConfig* config = configs->Get(uid);
  if (config == nullptr) {
    auto cfg = GetGemmConfig(debug_options, lhs, rhs, out, algorithm,
                             alpha_imag, alpha_real, lhs_batch, lhs_contract,
                             rhs_batch, rhs_contract);
    if (!cfg.ok()) return failure();
    config = configs->Set(uid, std::move(*cfg));
  }

  auto executed = RunGemm(*config, lhs_data, rhs_data, output_data, stream,
                          &scratch_allocator, nullptr);
  if (!executed.ok()) return failure();

  return success();
}

static bool Gemm(runtime::KernelContext* ctx, void** args, void** attrs) {
  static auto* handler =
      CustomCall::Bind("xla.gpu.gemm")
          .UserData<const ServiceExecutableRunOptions*>()
          .UserData<const DebugOptions*>()
          .UserData<JitRtGemmConfigCache*>()
          .Arg<jitrt::MemrefView>()  // lhs
          .Arg<jitrt::MemrefView>()  // rhs
          .Arg<jitrt::MemrefView>()  // out
          .Attr<int64_t>("algorithm")
          .Attr<double>("alpha_imag")
          .Attr<double>("alpha_real")
          .Attr<ArrayRef<int64_t>>("lhs_batching_dimensions")
          .Attr<ArrayRef<int64_t>>("lhs_contracting_dimensions")
          .Attr<ArrayRef<int64_t>>("rhs_batching_dimensions")
          .Attr<ArrayRef<int64_t>>("rhs_contracting_dimensions")
          .Attr<int64_t>("uid")
          .To<RuntimeChecks()>(Gemm::Handler())
          .release();

  return succeeded(handler->call(args, attrs, Executable::GetUserData(ctx)));
}

// -------------------------------------------------------------------------- //

namespace {
struct GemmBias {
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  LogicalResult operator()(const ServiceExecutableRunOptions* run_options,
                           const DebugOptions* debug_options,
                           JitRtGemmConfigCache* configs, jitrt::MemrefView lhs,
                           jitrt::MemrefView rhs, jitrt::MemrefView out,
                           jitrt::MemrefView bias, int64_t algorithm,
                           double alpha_imag, double alpha_real, double beta,
                           ArrayRef<int64_t> lhs_batch,
                           ArrayRef<int64_t> lhs_contract,
                           ArrayRef<int64_t> rhs_batch,
                           ArrayRef<int64_t> rhs_contract, int64_t uid) const;
  static GemmBias Handler() { return GemmBias(); }
};
}  // namespace

LogicalResult GemmBias::operator()(
    const ServiceExecutableRunOptions* run_options,
    const DebugOptions* debug_options, JitRtGemmConfigCache* configs,
    jitrt::MemrefView lhs, jitrt::MemrefView rhs, jitrt::MemrefView out,
    jitrt::MemrefView bias, int64_t algorithm, double alpha_imag,
    double alpha_real, double beta, ArrayRef<int64_t> lhs_batch,
    ArrayRef<int64_t> lhs_contract, ArrayRef<int64_t> rhs_batch,
    ArrayRef<int64_t> rhs_contract, int64_t uid) const {
  se::DeviceMemoryBase lhs_data = GetDeviceAddress(lhs);
  se::DeviceMemoryBase rhs_data = GetDeviceAddress(rhs);
  se::DeviceMemoryBase output_data = GetDeviceAddress(out);
  se::DeviceMemoryBase bias_data = GetDeviceAddress(bias);

  se::OwningScratchAllocator<> scratch_allocator(run_options->device_ordinal(),
                                                 run_options->allocator());

  VLOG(3) << "Running GEMM + Bias [beta=" << beta << "]";
  se::Stream* stream = run_options->stream();

  // Find the gemm config for this instance of operation based on uid.
  const GemmConfig* config = configs->Get(uid);
  if (config == nullptr) {
    auto cfg = GetGemmConfig(debug_options, lhs, rhs, out, algorithm,
                             alpha_imag, alpha_real, lhs_batch, lhs_contract,
                             rhs_batch, rhs_contract, beta);
    if (!cfg.ok()) return failure();
    config = configs->Set(uid, std::move(*cfg));
  }

  // Copy bias to the output buffer of they are different.
  if (out.data != bias.data)
    stream->ThenMemcpy(&output_data, bias_data, bias_data.size());

  auto executed = RunGemm(*config, lhs_data, rhs_data, output_data, stream,
                          &scratch_allocator, nullptr);
  if (!executed.ok()) return failure();

  return success();
}

static bool GemmBias(runtime::KernelContext* ctx, void** args, void** attrs) {
  static auto* handler =
      CustomCall::Bind("xla.gpu.gemm.bias")
          .UserData<const ServiceExecutableRunOptions*>()
          .UserData<const DebugOptions*>()
          .UserData<JitRtGemmConfigCache*>()
          .Arg<jitrt::MemrefView>()  // lhs
          .Arg<jitrt::MemrefView>()  // rhs
          .Arg<jitrt::MemrefView>()  // out
          .Arg<jitrt::MemrefView>()  // bias
          .Attr<int64_t>("algorithm")
          .Attr<double>("alpha_imag")
          .Attr<double>("alpha_real")
          .Attr<double>("beta")
          .Attr<ArrayRef<int64_t>>("lhs_batching_dimensions")
          .Attr<ArrayRef<int64_t>>("lhs_contracting_dimensions")
          .Attr<ArrayRef<int64_t>>("rhs_batching_dimensions")
          .Attr<ArrayRef<int64_t>>("rhs_contracting_dimensions")
          .Attr<int64_t>("uid")
          .To<RuntimeChecks()>(GemmBias::Handler())
          .release();

  return succeeded(handler->call(args, attrs, Executable::GetUserData(ctx)));
}

// -------------------------------------------------------------------------- //

SymbolMap JitRtCustomCallsSymbolMap(MangleAndInterner mangle) {
  SymbolMap symbol_map;

  auto bind = [&](llvm::StringRef name, auto symbol_ptr) {
    symbol_map[mangle(name)] = llvm::JITEvaluatedSymbol(
        llvm::pointerToJITTargetAddress(symbol_ptr), llvm::JITSymbolFlags());
  };

  bind("xla.gpu.func.launch", &xla::gpu::LaunchFunc);
  bind("xla.gpu.gemm", &xla::gpu::Gemm);
  bind("xla.gpu.gemm.bias", &xla::gpu::GemmBias);

  return symbol_map;
}

}  // namespace gpu
}  // namespace xla
