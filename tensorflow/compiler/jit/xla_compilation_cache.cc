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

#include "tensorflow/compiler/jit/xla_compilation_cache.h"

#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/status/status.h"
#include "absl/strings/str_join.h"
#include "absl/types/variant.h"
#include "tensorflow/compiler/jit/device_compilation_cache.h"
#include "tensorflow/compiler/jit/device_compilation_cluster_signature.h"
#include "tensorflow/compiler/jit/device_compilation_profiler.h"
#include "tensorflow/compiler/jit/device_compiler_client.h"
#include "tensorflow/compiler/jit/device_executable_persistor.h"
#include "tensorflow/compiler/jit/flags.h"
#include "tensorflow/compiler/jit/tf_graph_to_hlo_compiler.h"
#include "tensorflow/compiler/jit/xla_cluster_util.h"
#include "tensorflow/compiler/jit/xla_compile_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/tf2xla/xla_context.h"
#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/protobuf_util.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/graph_constructor.h"
#include "tensorflow/core/common_runtime/graph_optimizer.h"
#include "tensorflow/core/framework/attr_value_util.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/graph/node_builder.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/fingerprint.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/protobuf/debug_event.pb.h"
#include "tensorflow/core/protobuf/error_codes.pb.h"
#include "tensorflow/core/protobuf/graph_debug_info.pb.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/tpu/tpu_defs.h"
#include "tensorflow/core/util/dump_graph.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace tensorflow {
namespace {
// Print something that users can search for to definitively ascertain that XLA
// was used for their TF model.
//
// Prints only once to avoid spamming LOG(INFO).
void LogOnceXlaCompiledFirstCluster() {
  static absl::once_flag log_once;
  absl::call_once(log_once, [] {
    LOG(INFO) << "Compiled cluster using XLA!  This line is logged at most "
                 "once for the lifetime of the process.";
  });
}

Status EligibleToPersist(DeviceCompileState compile_state,
                         const xla::LocalExecutable* executable) {
  if (compile_state != DeviceCompileState::kCompiled) {
    return errors::FailedPrecondition(
        "Cache entry to serialize is not compiled.");
  }
  if (executable == nullptr) {
    return errors::FailedPrecondition(
        "LocalExecutable not found for cache entry to serialize.");
  }
  return OkStatus();
}
}  // namespace

XlaCompilationCache::XlaCompilationCache(
    std::unique_ptr<
        DeviceExecutablePersistor<xla::LocalExecutable, xla::LocalClient>>
        persistor,
    std::unique_ptr<
        DeviceCompilerClient<xla::LocalExecutable, xla::LocalClient>>
        compiler_client)
    : persistor_(std::move(persistor)),
      compiler_client_(std::move(compiler_client)) {
  cache_ = std::make_unique<DeviceCompilationCache<xla::LocalExecutable>>();
  async_compiler_threads_ = std::make_unique<tensorflow::thread::ThreadPool>(
      tensorflow::Env::Default(), "async_compiler_threads",
      kNumAsyncDeviceCompilerThreads);
}

XlaCompilationCache::~XlaCompilationCache() {
  // Since programs are owned by the cache, ensure any use of our programs have
  // completed by waiting for all stream executors to complete.
  compiler_client_->WaitForProgramsToFinish();
  // Wait for all outstanding compilations to finish.
  // Resetting the pointer explicitly in the top level destructor.
  // Without this, the pointer would be reset when the AsyncCompilationState
  // is destructed, which is dependent on the order of the members in the
  // XlaCompilationCache class, which is error prone if the order changes.
  async_compiler_threads_.reset();
  // TODO(b/110813685): Think about the program ownership model. Programs are
  // currently owned by the compilation cache which means we must wait for
  // program completion in the destructor. There are multiple compilation caches
  // around, which complicates things a little. Perhaps having programs be
  // shared_ptrs (an invasive change) would make the model easier to reason
  // about?
}

string XlaCompilationCache::DebugString() const {
  return "XLA JIT compilation cache";
}

Status XlaCompilationCache::CompileIfNeeded(
    const XlaCompiler::Options& options, const NameAttrList& function,
    const std::vector<XlaCompiler::Argument>& args,
    const XlaCompiler::CompileOptions& compile_options,
    DeviceCompileMode compile_mode, DeviceCompilationProfiler* profiler,
    const XlaCompiler::CompilationResult** out_compilation_result,
    xla::LocalExecutable** out_executable) {
  return CompileImpl(compile_options, options, function, args,
                     CompileScope::kFunction, compile_mode, /*ctx=*/nullptr,
                     profiler, out_compilation_result, out_executable);
}

Status XlaCompilationCache::CompileSingleOpIfNeeded(
    const XlaCompiler::Options& options,
    const std::vector<XlaCompiler::Argument>& args,
    const XlaCompiler::CompileOptions& compile_options, OpKernelContext* ctx,
    DeviceCompilationProfiler* profiler,
    const XlaCompiler::CompilationResult** out_compilation_result,
    xla::LocalExecutable** out_executable) {
  const NodeDef& def = ctx->op_kernel().def();
  NameAttrList name;
  name.set_name(def.op());
  *name.mutable_attr() = def.attr();
  // Remove the "_class" attribute from the attribute set used to create the
  // compilation cache key. This attribute is information for the colocator
  // and causes false uniqueness between nodes.
  name.mutable_attr()->erase("_class");
  return CompileImpl(compile_options, options, name, args, CompileScope::kOp,
                     DeviceCompileMode::kStrict, ctx, profiler,
                     out_compilation_result, out_executable);
}

StatusOr<DeviceCompilationCache<xla::LocalExecutable>::Value>
XlaCompilationCache::CompileStrict(
    const DeviceCompilationClusterSignature& sig,
    const XlaCompiler::CompileOptions& compile_options,
    const XlaCompiler::Options& options,
    const std::vector<XlaCompiler::Argument>& args,
    const NameAttrList& function,
    DeviceCompilationCache<xla::LocalExecutable>::Value cache_value,
    CompileScope scope, OpKernelContext* ctx,
    DeviceCompilationProfiler* profiler, mutex* mu) {
  tensorflow::Env* env = tensorflow::Env::Default();
  const uint64 compile_start_us = env->NowMicros();

  TfGraphToHloCompiler compiler(options);
  cache_value.compile_state = DeviceCompileState::kCompiled;

  std::unique_ptr<xla::LocalExecutable> out_executable;
  auto out_compilation_result =
      std::make_unique<XlaCompiler::CompilationResult>();

  if (scope == CompileScope::kOp) {
    cache_value.compilation_status = compiler.CompileSingleOp(
        compile_options, ctx, args, out_compilation_result.get());
  } else {
    CHECK(scope == CompileScope::kFunction);  // Crash OK
    cache_value.compilation_status = compiler.Compile(
        compile_options, function, args, out_compilation_result.get());
  }
  TF_RETURN_IF_ERROR(cache_value.compilation_status);
  TF_RET_CHECK(cache_value.executable == nullptr);
  TF_RET_CHECK(out_compilation_result->computation != nullptr);

  auto loaded_executable = persistor_->TryToLoadExecutable(
      DeviceCompilationClusterSignature::Hash()(sig), sig.HumanString(),
      options, *out_compilation_result, compiler_client_.get());

  if (loaded_executable.has_value()) {
    cache_value.compilation_status = loaded_executable->status();
    if (loaded_executable->ok()) {
      out_executable = *std::move(*loaded_executable);
    }
  } else {
    auto built_executable =
        compiler_client_->BuildExecutable(options, *out_compilation_result);
    cache_value.compilation_status = built_executable.status();
    if (built_executable.ok()) {
      out_executable = *std::move(built_executable);
    }

    TF_RETURN_IF_ERROR(
        EligibleToPersist(cache_value.compile_state, out_executable.get()));
    TF_RETURN_IF_ERROR(persistor_->TryToPersistExecutable(
        DeviceCompilationClusterSignature::Hash()(sig), sig.HumanString(),
        options, *out_compilation_result, *out_executable,
        compiler_client_.get()));
  }

  cache_value.compilation_result = out_compilation_result.get();
  cache_value.executable = out_executable.get();
  cache_->Store(sig, cache_value.compile_state, cache_value.compilation_status,
                std::move(out_compilation_result), std::move(out_executable));

  const uint64 compile_end_us = env->NowMicros();
  const uint64 compile_time_us = compile_end_us - compile_start_us;

  LogOnceXlaCompiledFirstCluster();
  TF_RETURN_IF_ERROR(profiler->RegisterCompilation(
      function, compile_time_us, loaded_executable.has_value()));
  return cache_value;
}

Status XlaCompilationCache::CompileAsynchronous(
    const DeviceCompilationClusterSignature& signature,
    const XlaCompiler::CompileOptions& compile_options,
    const XlaCompiler::Options& options,
    const std::vector<XlaCompiler::Argument>& args,
    const NameAttrList& function, CompileScope scope, OpKernelContext* ctx,
    DeviceCompilationProfiler* profiler) {
  // Explicitly capture all required data by value for async compilation.
  // Update compilation state in cache.
  cache_->Store(signature, DeviceCompileState::kCompiling, std::nullopt,
                std::nullopt, std::nullopt);
  profiler->IncrementOngoingAsyncCompilations();
  // Don't move the above code into the thread function as it synchronously
  // updates the async compilation state!

  // When the ThreadPool for the compilation cache is destroyed, it waits for
  // compilations to have finished. This means that both 'entry' and 'this' will
  // be alive for the duration of the compilation.
  // !!Pay attention when additional variables must be captured by this lambda!!
  // All values are captured by value. Make sure that all pointer values (like
  // entry) do not get freed until the lambda has finished.
  const std::string& function_name = function.name();
  async_compiler_threads_->Schedule([=] {
    VLOG(2) << "Starting asynchronous compilation of cluster " << function_name
            << '.';
    // We don't need to lock mu, but do it anyway to satisfy thread safety
    // analysis.
    mutex mu;
    mutex_lock lock(mu);
    auto s =
        CompileStrict(signature, compile_options, options, args, function,
                      DeviceCompilationCache<xla::LocalExecutable>::Value(),
                      scope, ctx, profiler, &mu);
    VLOG(2) << "Finished asynchronous compililation of cluster "
            << function_name << '.';
    profiler->DecrementOngoingAsyncCompilations();
    // Update compilation status in cache.
    if (!s.ok()) {
      cache_->Store(signature, std::nullopt, s.status(), std::nullopt,
                    std::nullopt);
    }
  });
  return OkStatus();
}

Status XlaCompilationCache::CompileImpl(
    const XlaCompiler::CompileOptions& compile_options,
    const XlaCompiler::Options& options, const NameAttrList& function,
    const std::vector<XlaCompiler::Argument>& args, CompileScope scope,
    DeviceCompileMode compile_mode, OpKernelContext* ctx,
    DeviceCompilationProfiler* profiler,
    const XlaCompiler::CompilationResult** out_compilation_result,
    xla::LocalExecutable** out_executable) {
  DCHECK_NE(out_executable, nullptr);
  VLOG(2) << "XlaCompilationCache::Compile " << DebugString();

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "num_inputs=" << args.size();
    for (int i = 0, end = args.size(); i < end; i++) {
      VLOG(3) << i << ": " << args[i].HumanString();
    }
  }
  TF_ASSIGN_OR_RETURN(auto signature,
                      DeviceCompilationClusterSignature::Build(function, args));

  // The outer lock protects the existence of the mutex in the map.
  mutex* cluster_mutex;
  {
    mutex_lock lock(cluster_mutexes_mu_);
    auto it =
        cluster_mutexes_.emplace(signature, std::make_unique<mutex>()).first;
    cluster_mutex = it->second.get();
  }

  profiler->RegisterExecution(function);

  string human_signature;
  if (VLOG_IS_ON(2)) {
    human_signature = VLOG_IS_ON(3) ? signature.HumanString() : function.name();
    VLOG(2) << "Signature: " << human_signature;
  }

  // Acquire the cache entry lock and compile, if necessary.
  // TODO(phawkins): this locking will need to be restructured when we implement
  // cache eviction.
  mutex_lock cluster_compile_lock(*cluster_mutex);
  auto cache_value = cache_->LookupOrCreate(signature);

  int64_t current_request_count = cache_value.request_count;
  VLOG(2) << "Compilation cache entry hit: "
          << static_cast<int>(cache_value.compile_state)
          << " signature: " << human_signature << " with request count "
          << current_request_count;

  DeviceCompileState state = cache_value.compile_state;
  *out_compilation_result = nullptr;
  *out_executable = nullptr;

  // Check if the requested entry is uncompiled and return an error if
  // compilation is disabled. This will raise an error for kLazy even if we have
  // not yet hit the compilation threshold and no compilation happens this
  // round. This is to avoid non-determanism of when compilation is disallowed,
  // for example by changing the threshold.
  if (state == DeviceCompileState::kUncompiled && FailOnXlaCompilation()) {
    VLOG(1) << "XLA compilation disabled: " << function.name() << "\n"
            << absl::StrJoin(
                   args, "\n",
                   [](std::string* out, const XlaCompiler::Argument& arg) {
                     absl::StrAppend(out, " arg: ", arg.HumanString());
                   });

    return errors::Internal("XLA compilation disabled");
  }

  if (state == DeviceCompileState::kUncompiled) {
    XLA_SCOPED_LOGGING_TIMER("Compilation of XLA executable");
    if (!profiler->ShouldCompileCluster(function, compile_mode,
                                        current_request_count)) {
      VLOG(2) << "Not compiling for signature: " << human_signature;
      return OkStatus();
    } else if (compile_mode == DeviceCompileMode::kAsync) {
      VLOG(2) << "Queueing asynchronous compilation for signature: "
              << human_signature;
      TF_RETURN_IF_ERROR(CompileAsynchronous(signature, compile_options,
                                             options, args, function, scope,
                                             ctx, profiler));
      return OkStatus();
    } else {
      VLOG(2) << "Instantly compiling for signature: " << human_signature;
      TF_ASSIGN_OR_RETURN(
          cache_value,
          CompileStrict(signature, compile_options, options, args, function,
                        cache_value, scope, ctx, profiler, cluster_mutex));
    }
  } else if (state == DeviceCompileState::kCompiling) {
    VLOG(2) << "Ongoing asynchronous compilation for signature: "
            << human_signature;
    return OkStatus();
  } else if (state == DeviceCompileState::kCompiled) {
    VLOG(2) << "Already Compiled for signature: " << human_signature;
  }

  TF_RETURN_IF_ERROR(cache_value.compilation_status);
  *out_compilation_result = cache_value.compilation_result;
  *out_executable = cache_value.executable;
  return OkStatus();
}

}  // namespace tensorflow
