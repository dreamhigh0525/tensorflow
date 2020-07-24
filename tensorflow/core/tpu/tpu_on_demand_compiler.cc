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
#include <memory>

#include "absl/types/span.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/hlo_cost_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_module_group.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/stream_executor/device_memory_allocator.h"
#include "tensorflow/stream_executor/tpu/c_api_conversions.h"
#include "tensorflow/stream_executor/tpu/proto_helper.h"
#include "tensorflow/stream_executor/tpu/status_helper.h"
#include "tensorflow/stream_executor/tpu/tpu_executor.h"
#include "tensorflow/stream_executor/tpu/tpu_executor_c_api.h"
#include "tensorflow/stream_executor/tpu/tpu_platform.h"
#include "tensorflow/stream_executor/tpu/tpu_stream.h"

namespace ApiConverter {
static SE_ExecutableRunOptions ToC(
    const xla::ServiceExecutableRunOptions& options) {
  SE_ExecutableRunOptions se_options;
  se_options.allocator = ApiConverter::ToC(options.run_options().allocator());
  se_options.device_ordinal = options.run_options().device_ordinal();
  auto impl =
      const_cast<stream_executor::Stream*>(options.stream())->implementation();
  se_options.stream = static_cast<TpuStream*>(impl)->se_stream();
  return se_options;
}
}  // namespace ApiConverter

namespace xla {

namespace {

using ::tensorflow::tpu::ExecutorApiFn;

class TpuExecutable : public Executable {
 public:
  TpuExecutable(SE_Executable* se_executable,
                std::shared_ptr<HloModule> hlo_module)
      : Executable(std::move(hlo_module), nullptr, nullptr),
        se_executable_(se_executable) {}

  ~TpuExecutable() override {
    ExecutorApiFn()->TpuExecutable_FreeFn(se_executable_);
  }

  StatusOr<ExecutionOutput> ExecuteAsyncOnStream(
      const ServiceExecutableRunOptions* run_options,
      std::vector<ExecutionInput> arguments,
      HloExecutionProfile* hlo_execution_profile) override {
    SE_ExecutableRunOptions se_run_options = ApiConverter::ToC(*run_options);
    SE_ExecutionInput** se_args = new SE_ExecutionInput*[arguments.size()];
    for (int i = 0; i < arguments.size(); ++i) {
      auto& arg = arguments[i];
      se_args[i] = new SE_ExecutionInput;

      ApiConverter::ToC(arg.shape(), &se_args[i]->shape_tree.shape);
      auto* arg_buffers = arg.MutableBuffers();
      absl::InlinedVector<SE_MaybeOwningDeviceMemory, 2> se_buffers;
      for (auto& pair : *arg_buffers) {
        se_buffers.push_back(ApiConverter::ToC(pair.second));
      }
      se_args[i]->shape_tree.buffers =
          new SE_MaybeOwningDeviceMemory[se_buffers.size()];
      for (int j = 0; j < se_buffers.size(); ++j) {
        se_args[i]->shape_tree.buffers[j] = se_buffers[j];
      }

      ApiConverter::ToC(arg.shape(), &se_args[i]->dynamic_shape);
      ApiConverter::ToC(arg.host_shape(), &se_args[i]->host_shape);
      const auto& unowned_indices = arg.unowned_indices();
      se_args[i]->unowned_indices_size = unowned_indices.size();
      se_args[i]->unowned_indices = new XLA_ShapeIndex[unowned_indices.size()];
      int j = 0;
      for (auto& idx : unowned_indices) {
        se_args[i]->unowned_indices[j] = ApiConverter::ToC(idx);
        ++j;
      }
    }
    SE_ExecutionOutput se_execution_output;
    StatusHelper status;
    ExecutorApiFn()->TpuExecutable_ExecuteAsyncOnStreamFn(
        se_executable_, &se_run_options, se_args, arguments.size(), nullptr,
        &se_execution_output, status.c_status);
    if (!status.ok()) {
      return status.status();
    }

    xla::ScopedShapedBuffer result(
        ApiConverter::FromC(&se_execution_output.result),
        run_options->stream()->parent()->GetAllocator());

    ExecutionOutput output(std::move(result));
    for (int i = 0; i < se_execution_output.aliased_indices_size; ++i) {
      output.AddAliasedIndex(
          ApiConverter::FromC(&se_execution_output.aliased_indices[i]));
    }

    for (int i = 0; i < se_execution_output.to_be_released_size; ++i) {
      output.AddToBeReleased(
          ApiConverter::FromC(&se_execution_output.to_be_released[i],
                              run_options->stream()->parent()->GetAllocator())
              .Release()
              .value());
    }

    return output;
  }

 private:
  SE_Executable* se_executable_;
};

XLA_HloModuleConfig HloModuleConfigToC(const xla::HloModuleConfig& config) {
  XLA_HloModuleConfig hlo_config;

  hlo_config.seed = config.seed();
  hlo_config.launch_id = config.launch_id();
  hlo_config.replica_count = config.replica_count();
  hlo_config.num_partitions = config.num_partitions();
  hlo_config.use_spmd_partitioning = config.use_spmd_partitioning();
  hlo_config.has_static_device_assignment =
      config.has_static_device_assignment();
  hlo_config.has_entry_computation_layout =
      config.has_entry_computation_layout();

  if (config.has_static_device_assignment()) {
    DeviceAssignmentProto dev_proto;
    config.static_device_assignment().Serialize(&dev_proto).IgnoreError();
    hlo_config.static_device_assignment =
        stream_executor::tpu::SerializeProto(dev_proto);
  }
  if (config.has_entry_computation_layout()) {
    auto layout = config.entry_computation_layout();
    ApiConverter::ToC(layout.result_layout().shape(),
                      &hlo_config.entry_computation_layout.result_layout);
    hlo_config.entry_computation_layout.parameter_layouts =
        new XLA_Shape[layout.parameter_count()];
    for (int i = 0; i < layout.parameter_count(); ++i) {
      ApiConverter::ToC(
          layout.parameter_layout(i).shape(),
          &hlo_config.entry_computation_layout.parameter_layouts[i]);
    }
    hlo_config.entry_computation_layout.parameter_count =
        layout.parameter_count();
  }
  return hlo_config;
}

class TpuCompiler : public Compiler {
 public:
  TpuCompiler() { compiler_ = ExecutorApiFn()->TpuCompiler_NewFn(); }
  ~TpuCompiler() override { ExecutorApiFn()->TpuCompiler_FreeFn(compiler_); }

  stream_executor::Platform::Id PlatformId() const override {
    return tensorflow::TpuPlatform::kId;
  }

  StatusOr<std::unique_ptr<HloModule>> RunHloPasses(
      std::unique_ptr<HloModule> module,
      stream_executor::StreamExecutor* executor,
      stream_executor::DeviceMemoryAllocator* device_allocator) override {
    XLA_HloModule hlo_module;
    hlo_module.module_config = HloModuleConfigToC(module->config());
    hlo_module.proto = stream_executor::tpu::SerializeProto(module->ToProto());
    auto allocator = ApiConverter::ToC(device_allocator);
    XLA_HloModule result;
    StatusHelper status;
    ExecutorApiFn()->TpuCompiler_RunHloPassesFn(
        compiler_, &hlo_module,
        static_cast<tensorflow::TpuExecutor*>(executor->implementation())
            ->se_executor(),
        &allocator, &result, status.c_status);
    if (!status.ok()) {
      return status.status();
    }
    HloModuleProto result_proto =
        stream_executor::tpu::DeserializeProto<HloModuleProto>(result.proto);
    return HloModule::CreateFromProto(result_proto, module->config());
  }

  StatusOr<
      std::tuple<std::unique_ptr<HloModule>, std::unique_ptr<BufferAssignment>>>
  RunHloPassesAndBufferAssignement(
      std::unique_ptr<HloModule> module,
      stream_executor::StreamExecutor* executor,
      stream_executor::DeviceMemoryAllocator* device_allocator) override {
    return Unimplemented(
        "This compiler does not support RunHloPassesAndBufferAssignment.");
  }

  StatusOr<std::unique_ptr<Executable>> RunBackend(
      std::unique_ptr<HloModule> module,
      stream_executor::StreamExecutor* executor,
      stream_executor::DeviceMemoryAllocator* device_allocator) override {
    XLA_HloModule hlo_module;
    hlo_module.module_config = HloModuleConfigToC(module->config());
    hlo_module.proto = stream_executor::tpu::SerializeProto(module->ToProto());
    auto allocator = ApiConverter::ToC(device_allocator);

    SE_Executable* result;
    StatusHelper status;
    ExecutorApiFn()->TpuCompiler_RunBackendFn(
        compiler_, &hlo_module,
        static_cast<tensorflow::TpuExecutor*>(executor->implementation())
            ->se_executor(),
        &allocator, &result, status.c_status);
    if (!status.ok()) {
      return status.status();
    }

    std::unique_ptr<Executable> exec =
        absl::make_unique<TpuExecutable>(result, std::move(module));
    return exec;
  }

  StatusOr<std::vector<std::unique_ptr<Executable>>> Compile(
      std::unique_ptr<HloModuleGroup> module_group,
      std::vector<std::vector<stream_executor::StreamExecutor*>> stream_exec,
      stream_executor::DeviceMemoryAllocator* device_allocator) override {
    XLA_HloModuleGroup se_module_group;
    se_module_group.proto =
        stream_executor::tpu::SerializeProto(module_group->ToProto());
    se_module_group.module_config =
        new XLA_HloModuleConfig[module_group->size()];
    for (int i = 0; i < module_group->size(); ++i) {
      const auto& config = module_group->module(i).config();
      se_module_group.module_config[i] = HloModuleConfigToC(config);
    }

    SE_StreamExecutorList* se_lists =
        new SE_StreamExecutorList[stream_exec.size()];
    for (int i = 0; i < stream_exec.size(); ++i) {
      se_lists[i].exec = new SE_StreamExecutor*[stream_exec[i].size()];
      for (int j = 0; j < stream_exec[i].size(); ++j) {
        se_lists[i].exec[j] = static_cast<tensorflow::TpuExecutor*>(
                                  stream_exec[i][j]->implementation())
                                  ->se_executor();
      }
    }

    SE_DeviceMemoryAllocator allocator = ApiConverter::ToC(device_allocator);

    SE_Executable** se_executables = new SE_Executable*[module_group->size()];

    StatusHelper status;

    ExecutorApiFn()->TpuCompiler_CompileFn(
        compiler_, &se_module_group, se_lists, stream_exec.size(), &allocator,
        se_executables, status.c_status);

    if (!status.ok()) {
      return status.status();
    }

    std::vector<std::unique_ptr<Executable>> executables;
    std::vector<std::unique_ptr<HloModule>> modules =
        module_group->ConsumeModules();
    for (int i = 0; i < module_group->size(); ++i) {
      executables[i] = absl::make_unique<TpuExecutable>(se_executables[i],
                                                        std::move(modules[i]));
    }

    return executables;
  }

  // Compiles the HLO module group for ahead-of-time execution.  This is
  // intended for use in static compilation.
  StatusOr<std::vector<std::unique_ptr<AotCompilationResult>>>
  CompileAheadOfTime(std::unique_ptr<HloModuleGroup> module_group,
                     const AotCompilationOptions& options) override {
    return Unimplemented("This compiler does not support CompileAheadOfTime.");
  }

  // Returns a function that computes the size in bytes of the logical
  // buffer that contains a shape.
  HloCostAnalysis::ShapeSizeFunction ShapeSizeBytesFunction() const override {
    return [this](const xla::Shape& shape) {
      XLA_Shape c_shape;
      ApiConverter::ToC(shape, &c_shape);
      int64 bytes =
          ExecutorApiFn()->TpuCompiler_ShapeSizeFn(compiler_, &c_shape);
      ApiConverter::Free(&c_shape);
      return bytes;
    };
  }

 private:
  Tpu_Compiler* compiler_;
};

static bool InitModule() {
  xla::Compiler::RegisterCompilerFactory(tensorflow::TpuPlatform::kId, []() {
    return absl::make_unique<TpuCompiler>();
  });
  return true;
}

static bool module_initialized = InitModule();

}  // namespace
}  // namespace xla
