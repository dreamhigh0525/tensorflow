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

// Defines the XlaCompileOnDemandOp.

#include "tensorflow/compiler/jit/xla_compile_on_demand_op.h"

#include "absl/memory/memory.h"
#include "tensorflow/compiler/jit/xla_device.h"
#include "tensorflow/compiler/jit/xla_launch_util.h"
#include "tensorflow/compiler/jit/xla_platform_info.h"
#include "tensorflow/compiler/tf2xla/const_analysis.h"
#include "tensorflow/compiler/tf2xla/tf2xla_util.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/lib/core/refcount.h"

namespace tensorflow {

// Returns argument indices corresponding to the resource variable inputs of
// kernel context `ctx`.
static std::vector<int> GetResourceVariableIndices(OpKernelContext* ctx) {
  std::vector<int> out;
  for (int64 i = 0; i < ctx->num_inputs(); i++) {
    if (ctx->input(i).dtype() == DT_RESOURCE) {
      out.push_back(i);
    }
  }
  return out;
}

Status XlaCompileOnDemandOp::Run(OpKernelContext* ctx,
                                 XlaCompilationCache* cache,
                                 const XlaCompiler::CompilationResult* result,
                                 xla::LocalExecutable* executable,
                                 const ResourceVarsSnapshot& variable_args) {
  xla::LocalClient* client = static_cast<xla::LocalClient*>(cache->client());

  XlaComputationLaunchContext launch_context(
      client, client->backend().memory_allocator(),
      client->default_device_ordinal(),
      /*allocate_xla_tensors=*/platform_info_.xla_device_metadata() != nullptr,
      platform_info_.xla_device_metadata()
          ? platform_info_.xla_device_metadata()->UseMultipleStreams()
          : false);

  std::map<int, const Tensor*> snapshot_ptrs;
  for (auto& p : variable_args) {
    snapshot_ptrs.emplace(p.first,
                          p.second.has_value() ? &p.second.value() : nullptr);
  }

  const xla::HloInputOutputAliasConfig& input_output_alias =
      executable->executable()->module().input_output_alias_config();
  xla::StatusOr<std::vector<xla::ExecutionInput>> execution_inputs =
      launch_context.PopulateInputs(ctx, result, snapshot_ptrs,
                                    /*missing_ctx_input_prefix=*/0,
                                    input_output_alias);
  TF_RETURN_IF_ERROR(execution_inputs.status());

  se::Stream* stream =
      ctx->op_device_context() ? ctx->op_device_context()->stream() : nullptr;

  VLOG(2) << "Executing computation: " << name();
  xla::ExecutableRunOptions run_options;
  run_options.set_stream(stream);
  run_options.set_allocator(client->backend().memory_allocator());
  run_options.set_intra_op_thread_pool(&ctx->eigen_cpu_device());
  run_options.set_rng_seed(GetXLARandomSeed());

  xla::StatusOr<xla::ExecutionOutput> run_result =
      executable->Run(execution_inputs.ConsumeValueOrDie(), run_options);
  TF_RETURN_IF_ERROR(run_result.status());
  xla::ExecutionOutput execution_output = run_result.ConsumeValueOrDie();
  xla::StatusOr<std::vector<VariableInfo>> variable_infos =
      GatherVariableInfo(ctx, *result, 0);
  TF_RETURN_IF_ERROR(variable_infos.status());
  TF_RETURN_IF_ERROR(LockVariables(absl::MakeSpan(*variable_infos)));
  TF_RETURN_IF_ERROR(launch_context.PopulateOutputs(
      ctx, result, execution_output.ConsumeResult(),
      /*missing_ctx_input_prefix=*/0, absl::MakeSpan(*variable_infos),
      input_output_alias, snapshot_ptrs));
  return Status::OK();
}

Status XlaCompileOnDemandOp::Compile(
    OpKernelContext* ctx, const XlaCompiler::CompilationResult** result,
    XlaCompilationCache** cache, ResourceVarsSnapshot* variable_args,
    xla::LocalExecutable** executable) {
  std::map<int, Tensor> constant_arguments;

  std::vector<int> constant_input_indices;
  TF_RETURN_IF_ERROR(GetCompileTimeConstInputs(
      &ctx->op_kernel(), &constant_input_indices, ctx->function_library()));
  CHECK(absl::c_is_sorted(constant_input_indices));

  for (int64 i = 0; i < ctx->num_inputs(); ++i) {
    const Tensor& device_tensor = ctx->input(i);
    if (const XlaTensor* xla_tensor = XlaTensor::FromTensor(&device_tensor)) {
      if (xla_tensor->has_host_tensor()) {
        if (absl::c_binary_search(constant_input_indices, i)) {
          constant_arguments[i] = xla_tensor->host_tensor();
        }
      }
    }

    if (!constant_arguments.count(i)) {
      if (absl::c_binary_search(constant_input_indices, i)) {
        // Slow path; the argument is not available as a host constant so we
        // must fetch it synchronously.
        Tensor host_tensor;
        AllocatorAttributes attrs;
        attrs.set_on_host(true);
        TF_RETURN_IF_ERROR(ctx->allocate_temp(
            device_tensor.dtype(), device_tensor.shape(), &host_tensor, attrs));
        Status status = ctx->op_device_context()->CopyDeviceTensorToCPUSync(
            &device_tensor, "ConstantArgument",
            reinterpret_cast<Device*>(ctx->device()), &host_tensor);
        if (!status.ok()) {
          LOG(ERROR) << "Copying tensor of shape "
                     << device_tensor.shape().DebugString() << " from "
                     << ctx->device()->name() << "to CPU failed with "
                     << status.ToString();
          return status;
        }
        constant_arguments[i] = host_tensor;
      }
    }
  }

  // We store information about the JIT-compiled XLA computation
  // in the ResourceMgr.
  ResourceMgr* rm = ctx->resource_manager();
  CHECK(rm);

  TF_RETURN_IF_ERROR(rm->LookupOrCreate<XlaCompilationCache>(
      rm->default_container(), "xla_cache", cache,
      [&](XlaCompilationCache** write_into_cache) {
        return BuildXlaCompilationCache(ctx, platform_info_, write_into_cache);
      }));

  absl::optional<se::TfAllocatorAdapter> tf_allocator_adapter;
  XlaCompiler::Options options =
      GenerateCompilerOptions(*cache, ctx, platform_info_,
                              /*has_ref_vars=*/true, &tf_allocator_adapter);

  XlaCompiler::CompileOptions compile_options;
  compile_options.is_entry_computation = true;
  // Optimization: where possible, have the computation return a naked array
  // rather than a one-element tuple.
  compile_options.always_return_tuple = false;

  std::vector<int> variables_indices = GetResourceVariableIndices(ctx);
  std::vector<XlaCompiler::Argument> args;
  {
    std::vector<VariableInfo> variable_infos;
    TF_RETURN_IF_ERROR(
        GetVariableInfosFromCtxInputs(ctx, variables_indices, &variable_infos));
    TF_RETURN_IF_ERROR(LockVariables(absl::MakeSpan(variable_infos)));
    TF_RETURN_IF_ERROR(SnapshotResourceVariables(
        ctx, variables_indices, variable_infos, variable_args));
    TF_RETURN_IF_ERROR(XlaComputationLaunchContext::BuildXlaCompilerArguments(
        constant_arguments, variable_infos, ctx, &args));
  }

  return (*cache)->CompileSingleOp(options, args, ctx, compile_options, result,
                                   executable);
}

void XlaCompileOnDemandOp::Compute(OpKernelContext* ctx) {
  const XlaCompiler::CompilationResult* result;
  xla::LocalExecutable* executable;
  ResourceVarsSnapshot variable_args;
  XlaCompilationCache* cache;
  OP_REQUIRES_OK(ctx,
                 Compile(ctx, &result, &cache, &variable_args, &executable));

  // Hold the reference to the JIT during evaluation. (We could probably
  // free it sooner because the ResourceMgr will retain a reference, but
  // this is more obviously correct.)
  core::ScopedUnref cache_ref(cache);
  OP_REQUIRES_OK(ctx, Run(ctx, cache, result, executable, variable_args));
}

}  // namespace tensorflow
