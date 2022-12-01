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
#include <string>
#include <utility>
#include <variant>

#include "tensorflow/compiler/mlir/tf2xla/mlir_bridge_rollout_policy.h"
#include "absl/base/call_once.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/variant.h"
#include "tensorflow/compiler/jit/device_compilation_profiler.h"
#include "tensorflow/compiler/jit/flags.h"
#include "tensorflow/compiler/jit/tf_graph_to_hlo_compiler.h"
#include "tensorflow/compiler/jit/xla_cluster_util.h"
#include "tensorflow/compiler/jit/xla_compilation_cache.pb.h"
#include "tensorflow/compiler/jit/xla_compile_util.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/compile_mlir_util.h"
#include "tensorflow/compiler/mlir/utils/array_container_utils.h"
#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/tf2xla/xla_context.h"
#include "tensorflow/compiler/xla/client/client_library.h"
#include "tensorflow/compiler/xla/protobuf_util.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
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
#include "tensorflow/core/lib/strings/proto_serialization.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/fingerprint.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/protobuf/debug_event.pb.h"
#include "tensorflow/core/protobuf/error_codes.pb.h"
#include "tensorflow/core/protobuf/graph_debug_info.pb.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/tpu/tpu_defs.h"
#include "tensorflow/core/util/determinism.h"
#include "tensorflow/core/util/dump_graph.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace tensorflow {
namespace {

using TensorTypeAndShape = XlaCompilationCache::Signature::TensorTypeAndShape;

constexpr char kXlaSerializedCacheKeySeparator[] = "__";

// Functor that converts a Signature's arg to a human readable string.
struct SignatureHumanStringAppender {
  explicit SignatureHumanStringAppender(string* dest) : dest(dest) {}
  string* dest;
  void operator()(const Tensor& arg) {
    absl::StrAppend(dest, "; ", arg.DebugString());
  }
  void operator()(const TensorTypeAndShape& arg) {
    absl::StrAppend(dest, ",", DataTypeString(arg.first));
    absl::StrAppend(dest, " [", absl::StrJoin(arg.second, ","), "]");
  }
};

// Functor that compares the arg values of two different signatures. Returns
// true when the args are not equal.
struct SignatureNotEqual {
  bool operator()(const Tensor& arg, const Tensor& other) {
    return arg.dtype() != other.dtype() || arg.shape() != other.shape() ||
           arg.tensor_data() != other.tensor_data();
  }
  bool operator()(const TensorTypeAndShape& arg,
                  const TensorTypeAndShape& other) {
    return arg.first != other.first || arg.second != other.second;
  }
  bool operator()(const Tensor& arg, const TensorTypeAndShape& other) {
    return true;
  }
  bool operator()(const TensorTypeAndShape& arg, const Tensor& other) {
    return true;
  }
};

// Functor that incrementally computes a Signature's hash given its current hash
// and one of its args.
struct SignatureHashCombiner {
  explicit SignatureHashCombiner(const uint64 h) : h(h) {}
  uint64 h;
  uint64 operator()(const Tensor& arg) {
    h = Hash64Combine(h, std::hash<int>()(static_cast<int>(arg.dtype())));
    h = Hash64Combine(
        h, Hash64(arg.tensor_data().data(), arg.tensor_data().size()));
    for (int dim = 0; dim < arg.dims(); ++dim) {
      h = Hash64Combine(h, std::hash<int>()(arg.dim_size(dim)));
    }
    return h;
  }
  uint64 operator()(const TensorTypeAndShape& arg) {
    h = Hash64Combine(h, std::hash<int>()(static_cast<int>(arg.first)));
    h = Hash64Combine(h, std::hash<int>()(arg.second.size()));
    for (int dim : arg.second) {
      h = Hash64Combine(h, std::hash<int>()(dim));
    }
    return h;
  }
};

std::string XlaSerializedCacheKeyToString(const XlaSerializedCacheKey& key) {
  return absl::StrCat(
      key.prefix(), key.prefix().empty() ? "" : kXlaSerializedCacheKeySeparator,
      key.signature_fingerprint(), kXlaSerializedCacheKeySeparator,
      key.cluster_fingerprint(), kXlaSerializedCacheKeySeparator,
      key.device_type());
}

}  // namespace

XlaCompilationCache::XlaCompilationCache(Config config,
                                         xla::LocalClient* client,
                                         DeviceType device_type)
    : client_(client),
      device_type_(std::move(device_type)),
      disable_strict_signature_checks_(config.disable_strict_signature_checks),
      persistance_prefix_(config.persistance_prefix),
      persistent_cache_directory_(config.persistent_cache_directory) {
  async_compiler_threads_ = std::make_unique<tensorflow::thread::ThreadPool>(
      tensorflow::Env::Default(), "async_compiler_threads",
      kNumAsyncDeviceCompilerThreads);
}

XlaCompilationCache::~XlaCompilationCache() {
  // Ensure any use of our programs have completed by waiting for all stream
  // executors to complete.
  for (auto* executor : client_->backend().stream_executors()) {
    bool ok = executor->SynchronizeAllActivity();
    if (!ok) {
      LOG(ERROR) << "Error synchronizing activity while waiting for all "
                    "programs to complete";
    }
  }
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

// Compute a string signature which encodes the shapes of the
// arguments in the supplied list.
string XlaCompilationCache::Signature::HumanString() const {
  string result = name;
  for (const auto& a : args) {
    absl::visit(SignatureHumanStringAppender(&result), a);
  }
  return result;
}

bool XlaCompilationCache::Signature::operator==(const Signature& other) const {
  if (name != other.name) return false;
  if (args.size() != other.args.size()) return false;
  for (int i = 0, end = args.size(); i < end; ++i) {
    if (absl::visit(SignatureNotEqual(), args[i], other.args[i])) {
      return false;
    }
  }
  return true;
}

uint64 XlaCompilationCache::Signature::Hash::operator()(
    const XlaCompilationCache::Signature& signature) const {
  uint64 h = std::hash<string>()(signature.name);
  for (const auto& arg : signature.args) {
    h = absl::visit(SignatureHashCombiner(h), arg);
  }
  return h;
}

StatusOr<XlaCompilationCache::Signature> XlaCompilationCache::BuildSignature(
    const NameAttrList& function,
    absl::Span<const XlaCompiler::Argument> args) {
  Signature signature;
  signature.name = Canonicalize(function.name(), AttrSlice(&function.attr()));

  for (const XlaCompiler::Argument& arg : args) {
    switch (arg.kind) {
      case XlaCompiler::Argument::kConstant:
      case XlaCompiler::Argument::kConstantResource:
        signature.args.push_back(arg.constant_value);
        break;
      case XlaCompiler::Argument::kParameter:
      case XlaCompiler::Argument::kResource:
        signature.args.push_back(
            TensorTypeAndShape(arg.type, arg.DimensionSizesAsInlinedVector()));
        break;
      default:
        return errors::InvalidArgument(
            "Unhandled argument kind in XlaCompilationCache: ",
            arg.HumanString());
    }
  }
  return std::move(signature);
}

static std::vector<const xla::Shape*> GetShapePointers(
    absl::Span<const xla::Shape> shapes) {
  std::vector<const xla::Shape*> shape_ptrs;
  shape_ptrs.reserve(shapes.size());
  for (const auto& shape : shapes) {
    shape_ptrs.push_back(&shape);
  }
  return shape_ptrs;
}

Status XlaCompilationCache::BuildExecutable(
    const XlaCompiler::Options& options,
    const XlaCompiler::CompilationResult& result,
    std::unique_ptr<xla::LocalExecutable>* executable) {
  VLOG(2) << "Compiling to local executable";

  std::vector<const xla::Shape*> argument_layouts =
      GetShapePointers(result.xla_input_shapes);
  xla::ExecutableBuildOptions build_options = GetExecutableBuildOptions(
      options, result, client_->default_device_ordinal());
  TF_ASSIGN_OR_RETURN(
      auto executables,
      client_->Compile(*result.computation, argument_layouts, build_options));
  TF_RET_CHECK(executables.size() == 1);
  *executable = std::move(executables[0]);
  return OkStatus();
}

StatusOr<std::unique_ptr<xla::AotCompilationResult>>
XlaCompilationCache::BuildSerializedExecutable(
    const XlaCompiler::Options& options,
    const XlaCompiler::CompilationResult& result) {
  VLOG(2) << "Compiling to local executable";

  std::vector<const xla::Shape*> argument_layouts =
      GetShapePointers(result.xla_input_shapes);
  xla::ExecutableBuildOptions build_options = GetExecutableBuildOptions(
      options, result, client_->default_device_ordinal());
  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<xla::AotCompilationResult>> aot_results,
      client_->CompileAheadOfTime(*result.computation, argument_layouts,
                                  build_options));
  TF_RET_CHECK(aot_results.size() == 1);
  return std::move(aot_results[0]);
}

StatusOr<std::unique_ptr<xla::LocalExecutable>>
XlaCompilationCache::LoadExecutable(
    const XlaCompiler::Options& options,
    const XlaCompiler::CompilationResult& result,
    const std::string& serialized_aot_result) {
  VLOG(2) << "Loading local executable from serialized AOT result";

  xla::ExecutableBuildOptions build_options = GetExecutableBuildOptions(
      options, result, client_->default_device_ordinal());
  return client_->Load(serialized_aot_result, build_options);
}

Status XlaCompilationCache::Compile(
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

Status XlaCompilationCache::CompileSingleOp(
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
}  // namespace

Status XlaCompilationCache::CompileStrict(
    const Signature& sig, const XlaCompiler::CompileOptions& compile_options,
    const XlaCompiler::Options& options,
    const std::vector<XlaCompiler::Argument>& args,
    const NameAttrList& function, CompileScope scope, OpKernelContext* ctx,
    DeviceCompilationProfiler* profiler, Entry* entry) {
  tensorflow::Env* env = tensorflow::Env::Default();
  const uint64 compile_start_us = env->NowMicros();

  TfGraphToHloCompiler compiler(options);
  entry->compile_state = CompileState::kCompiled;
  if (scope == CompileScope::kOp) {
    entry->compilation_status = compiler.CompileSingleOp(
        compile_options, ctx, args, &entry->compilation_result);
  } else {
    CHECK(scope == CompileScope::kFunction);  // Crash OK
    entry->compilation_status = compiler.Compile(
        compile_options, function, args, &entry->compilation_result);
  }
  TF_RETURN_IF_ERROR(entry->compilation_status);
  TF_RET_CHECK(entry->executable.get() == nullptr);
  TF_RET_CHECK(entry->compilation_result.computation != nullptr);

  std::optional<XlaSerializedCacheEntry> serialized_entry;
  if (!persistent_cache_directory_.empty()) {
    const xla::HloModuleProto& hlo_module =
        entry->compilation_result.computation->proto();

    XlaSerializedCacheKey cache_key = BuildSerializedCacheKey(sig, hlo_module);

    {
      XLA_SCOPED_LOGGING_TIMER(absl::StrCat(
          "Try loading serialized cache entry:", sig.HumanString()));
      TF_ASSIGN_OR_RETURN(serialized_entry, TryLoadSerializedEntry(cache_key));
    }

    if (serialized_entry.has_value()) {
      TF_RETURN_IF_ERROR(
          VerifyLoadedCacheEntry(cache_key, hlo_module, *serialized_entry));
    }
  }

  if (serialized_entry.has_value()) {
    VLOG(1) << "Loading cached entry for: " << sig.HumanString();
    StatusOr<std::unique_ptr<xla::LocalExecutable>> executable = LoadExecutable(
        options, entry->compilation_result, serialized_entry->executable());
    entry->compilation_status = executable.status();
    if (executable.ok()) {
      entry->executable = *std::move(executable);
    }
  } else {
    entry->compilation_status =
        BuildExecutable(options, entry->compilation_result, &entry->executable);

    // Caching is done regardless of the entry->compilation_status. To take
    // advantage of newer compilation code, a cache flush is required.
    if (!persistent_cache_directory_.empty()) {
      XLA_SCOPED_LOGGING_TIMER(absl::StrCat(
          "Serializing and saving cache entry: ", sig.HumanString()));
      TF_ASSIGN_OR_RETURN(XlaSerializedCacheEntry serialized_entry,
                          SerializeEntry(options, sig, *entry));
      TF_RETURN_IF_ERROR(SaveSerializedEntry(std::move(serialized_entry)));
    }
  }

  const uint64 compile_end_us = env->NowMicros();
  const uint64 compile_time_us = compile_end_us - compile_start_us;

  LogOnceXlaCompiledFirstCluster();
  return profiler->RegisterCompilation(function, compile_time_us,
                                       serialized_entry.has_value());
}

Status XlaCompilationCache::CompileAsynchronous(
    const Signature& signature,
    const XlaCompiler::CompileOptions& compile_options,
    const XlaCompiler::Options& options,
    const std::vector<XlaCompiler::Argument>& args,
    const NameAttrList& function, CompileScope scope, OpKernelContext* ctx,
    DeviceCompilationProfiler* profiler, Entry* entry) {
  // Explicitly capture all required data by value for async compilation.
  entry->compile_state = CompileState::kCompiling;
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
    Entry local_entry;
    VLOG(2) << "Starting asynchronous compilation of cluster " << function_name
            << '.';
    // We don't need to lock local_entry.mu, but do it anyway to satisfy
    // thread safety analysis.
    mutex_lock entry_lock(local_entry.mu);
    Status s = CompileStrict(signature, compile_options, options, args,
                             function, scope, ctx, profiler, &local_entry);
    VLOG(2) << "Finished asynchronous compililation of cluster "
            << function_name << '.';
    profiler->DecrementOngoingAsyncCompilations();
    {  // Populate original entry with compilation result.
      mutex_lock entry_lock(entry->mu);
      if (!s.ok()) {
        entry->compilation_status = s;
      } else {
        entry->compilation_status = local_entry.compilation_status;
      }
      entry->compilation_result = local_entry.compilation_result;
      entry->compile_state = local_entry.compile_state;
      entry->executable = std::move(local_entry.executable);
    }
  });
  return OkStatus();
}

StatusOr<XlaCompilationCache::CompilationResultAndExecutable>
XlaCompilationCache::GetCompilationResultIfAlreadyCompiled(
    const NameAttrList& function,
    absl::Span<const XlaCompiler::Argument> args) {
  CompilationResultAndExecutable result{nullptr, nullptr};

  TF_ASSIGN_OR_RETURN(Signature signature, BuildSignature(function, args));

  // The outer lock protects the existence of the cache entry. It does not
  // protect the contents of the cache entry.
  Entry* entry;
  {
    mutex_lock lock(compile_cache_mu_);
    // Try to find a cache entry.
    auto cache_entry = cache_.find(signature);
    if (cache_entry == cache_.end()) {
      return result;
    }
    entry = cache_entry->second.get();
  }

  // Acquire the cache entry lock.
  // TODO(phawkins): this locking will need to be restructured when we implement
  // cache eviction.
  mutex_lock entry_lock(entry->mu);

  const CompileState state = entry->compile_state;
  if (state != CompileState::kCompiled) {
    return result;
  }

  int64_t current_request_count = ++entry->request_count;

  VLOG(2) << "Compilation cache entry hit and is already compiled : "
          << static_cast<int>(entry->compile_state)
          << " signature: " << signature.HumanString() << " with request count "
          << current_request_count;

  result.compilation_result = &entry->compilation_result;
  result.executable = entry->executable.get();

  return result;
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
  TF_ASSIGN_OR_RETURN(Signature signature, BuildSignature(function, args));

  // The outer lock protects the existence of the cache entry. It does not
  // protect the contents of the cache entry.
  Entry* entry;
  {
    mutex_lock lock(compile_cache_mu_);
    // Find or create a cache entry.
    auto cache_entry = cache_.find(signature);
    if (cache_entry == cache_.end()) {
      auto inserted_entry =
          cache_.emplace(signature, std::make_unique<Entry>());
      cache_entry = inserted_entry.first;
    }
    entry = cache_entry->second.get();
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
  mutex_lock entry_lock(entry->mu);
  int64_t current_request_count = ++entry->request_count;
  VLOG(2) << "Compilation cache entry hit: "
          << static_cast<int>(entry->compile_state)
          << " signature: " << human_signature << " with request count "
          << current_request_count;

  CompileState state = entry->compile_state;
  *out_compilation_result = nullptr;
  *out_executable = nullptr;

  // Check if the requested entry is uncompiled and return an error if
  // compilation is disabled. This will raise an error for kLazy even if we have
  // not yet hit the compilation threshold and no compilation happens this
  // round. This is to avoid non-determanism of when compilation is disallowed,
  // for example by changing the threshold.
  if (state == CompileState::kUncompiled && FailOnXlaCompilation()) {
    VLOG(1) << "XLA compilation disabled: " << function.name() << "\n"
            << absl::StrJoin(
                   args, "\n",
                   [](std::string* out, const XlaCompiler::Argument& arg) {
                     absl::StrAppend(out, " arg: ", arg.HumanString());
                   });

    return errors::Internal("XLA compilation disabled");
  }

  if (state == CompileState::kUncompiled) {
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
                                             ctx, profiler, entry));
      return OkStatus();
    } else {
      VLOG(2) << "Instantly compiling for signature: " << human_signature;
      TF_RETURN_IF_ERROR(CompileStrict(signature, compile_options, options,
                                       args, function, scope, ctx, profiler,
                                       entry));
    }
  } else if (state == CompileState::kCompiling) {
    VLOG(2) << "Ongoing asynchronous compilation for signature: "
            << human_signature;
    return OkStatus();
  } else if (state == CompileState::kCompiled) {
    VLOG(2) << "Already Compiled for signature: " << human_signature;
  }

  TF_RETURN_IF_ERROR(entry->compilation_status);
  *out_compilation_result = &entry->compilation_result;
  *out_executable = entry->executable.get();
  return OkStatus();
}

XlaSerializedCacheKey XlaCompilationCache::BuildSerializedCacheKey(
    const Signature& sig, const xla::HloModuleProto& hlo_module) const {
  XlaSerializedCacheKey serialized_cache_key;
  serialized_cache_key.set_signature_fingerprint(Signature::Hash()(sig));
  serialized_cache_key.set_cluster_fingerprint(
      DeterministicProtoHash64(hlo_module));
  serialized_cache_key.set_device_type(device_type_.type_string());
  serialized_cache_key.set_prefix(persistance_prefix_);
  return serialized_cache_key;
}

Status XlaCompilationCache::VerifyLoadedCacheEntry(
    const XlaSerializedCacheKey& key, const xla::HloModuleProto& hlo_module,
    const XlaSerializedCacheEntry& entry) {
  XLA_SCOPED_LOGGING_TIMER(absl::StrCat("Verifying loaded cache entry: ",
                                        hlo_module.entry_computation_name()));

  if (!AreSerializedProtosEqual(key, entry.key())) {
    VLOG(2) << "Serialized cache key does not match:\n"
            << "got:\n"
            << entry.key().DebugString() << "\nexpected:\n"
            << key.DebugString() << "\n";
    return errors::InvalidArgument("Serialized cache key does not match.");
  }

  // Perform a stricter (slower) check of the snapshot to verify that they
  // match exactly.
  if (!disable_strict_signature_checks_) {
    if (!AreSerializedProtosEqual(hlo_module, entry.hlo_module())) {
      VLOG(2) << "HLOs do not match:\n"
              << "got:\n"
              << hlo_module.DebugString() << "\nexpected:\n"
              << entry.hlo_module().DebugString() << "\n";
      return errors::InvalidArgument("Serialized HLO does not match.");
    }
  }

  if (entry.executable().empty()) {
    return errors::InvalidArgument("No binary found in serialized entry.");
  }
  return OkStatus();
}

StatusOr<XlaSerializedCacheEntry> XlaCompilationCache::SerializeEntry(
    const XlaCompiler::Options& options, const Signature& sig,
    const Entry& entry) {
  if (entry.compile_state != CompileState::kCompiled) {
    return errors::FailedPrecondition(
        "Cache entry to serialize is not compiled.");
  }
  if (entry.executable == nullptr) {
    return errors::FailedPrecondition(
        "LocalExecutable not found for cache entry to serialize.");
  }
  if (entry.executable->executable() == nullptr) {
    return errors::FailedPrecondition(
        "Executable not found for cache entry to serialize.");
  }

  XlaSerializedCacheEntry serialized_entry;
  const xla::HloModuleProto& hlo_module =
      entry.compilation_result.computation->proto();
  *serialized_entry.mutable_key() = BuildSerializedCacheKey(sig, hlo_module);
  *serialized_entry.mutable_hlo_module() = hlo_module;

  // XLA compiler supports exporting executables as an AOT compilation result
  // to avoid running potentially expensive compilation pipeline twice. If entry
  // does not have an executable, only then we'll run the AOT compiler.
  std::unique_ptr<xla::AotCompilationResult> aot_result;

  // Check if XLA compiler can export available executable.
  if (entry.executable) {
    VLOG(1) << "Export local executable as an AOT compilation result";
    xla::Compiler* compiler = client_->backend().compiler();
    auto exported = compiler->Export(entry.executable->executable());
    if (exported.ok()) {
      aot_result = std::move(*exported);
    } else if (exported.status().code() == error::UNIMPLEMENTED) {
      VLOG(1) << "Executable export is not implemented";
    } else {
      return exported.status();
    }
  }

  // Run AOT compilation pipeline only if execuable export is not supported.
  if (!aot_result) {
    VLOG(1) << "Compile executable using AOT compilation pipeline";
    TF_ASSIGN_OR_RETURN(aot_result, BuildSerializedExecutable(
                                        options, entry.compilation_result));
  }

  TF_ASSIGN_OR_RETURN(std::string serialized, aot_result->SerializeAsString());
  serialized_entry.set_executable(std::move(serialized));
  return serialized_entry;
}

namespace {

std::string GetFilePath(const XlaSerializedCacheKey& key,
                        absl::string_view persistent_cache_directory) {
  const std::string file_name =
      absl::StrCat(XlaSerializedCacheKeyToString(key), ".pb");
  return io::JoinPath(persistent_cache_directory, file_name);
}

}  // namespace

Status XlaCompilationCache::SaveSerializedEntry(
    const XlaSerializedCacheEntry& entry) {
  Env* env = Env::Default();
  TF_RETURN_IF_ERROR(env->RecursivelyCreateDir(persistent_cache_directory_));
  const std::string file_path =
      GetFilePath(entry.key(), persistent_cache_directory_);
  return WriteBinaryProto(env, file_path, entry);
}

StatusOr<std::optional<XlaSerializedCacheEntry>>
XlaCompilationCache::TryLoadSerializedEntry(const XlaSerializedCacheKey& key) {
  Env* env = Env::Default();
  const std::string file_path = GetFilePath(key, persistent_cache_directory_);
  if (!env->FileExists(file_path).ok()) {
    return StatusOr<std::optional<XlaSerializedCacheEntry>>(std::nullopt);
  }

  XlaSerializedCacheEntry entry;
  TF_RETURN_IF_ERROR(ReadTextOrBinaryProto(env, file_path, &entry));
  return StatusOr<std::optional<XlaSerializedCacheEntry>>(entry);
}

xla::ExecutableBuildOptions GetExecutableBuildOptions(
    const XlaCompiler::Options& options,
    const XlaCompiler::CompilationResult& result, int default_device_ordinal) {
  xla::ExecutableBuildOptions build_options;
  if (result.collective_info) {
    build_options.set_num_replicas(result.collective_info->group_size);
  }
  if (options.device_ordinal != -1) {
    build_options.set_device_ordinal(options.device_ordinal);
  } else if (default_device_ordinal != -1) {
    build_options.set_device_ordinal(default_device_ordinal);
  }
  build_options.set_result_layout(result.xla_output_shape);
  build_options.set_device_allocator(options.device_allocator.get());
  build_options.set_alias_passthrough_params(options.alias_passthrough_params);
  build_options.mutable_debug_options()->set_xla_detailed_logging_and_dumping(
      options.detailed_logging);
  if (tensorflow::OpDeterminismRequired()) {
    build_options.mutable_debug_options()->set_xla_gpu_deterministic_ops(true);
  }
  return build_options;
}

}  // namespace tensorflow
