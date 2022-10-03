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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_CPU_CPU_EXECUTABLE_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_CPU_CPU_EXECUTABLE_H_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "tensorflow/compiler/xla/runtime/executable.h"
#include "tensorflow/compiler/xla/runtime/jit_executable.h"
#include "tensorflow/compiler/xla/service/buffer_assignment.h"
#include "tensorflow/compiler/xla/service/cpu/simple_orc_jit.h"
#include "tensorflow/compiler/xla/service/custom_call_status_internal.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/hlo_dataflow_analysis.h"
#include "tensorflow/compiler/xla/service/hlo_execution_profile.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/stream_executor/device_memory_allocator.h"
#include "tensorflow/compiler/xla/stream_executor/stream_executor.h"
#include "tensorflow/compiler/xla/types.h"

namespace xla {
namespace cpu {

// Maps the descriptor table with inputs/outputs. Note that flattened_outputs
// and result are mutually exclusive -- see below.
//
// Contains the same info as "xla_framework" MLIR annotations. That is:
// - inputs: indices in the descriptor table of the input arguments.
// - output_is_tuple: if set, the output is a tuple.
// - flattened_outputs: if the output is a tuple, this contains the indices
//   (if any) in the descriptor table that correspond to the expanded tuple.
// - result: if the output is NOT a tuple, contains the index in the descriptor
//   table of the result.
struct XlaFrameworkMapping {
  std::vector<int64_t> inputs;
  std::vector<int64_t> flattened_outputs;
  int64_t result = -1;
  bool output_is_tuple = false;
};

// BufferDesc for passing raw `buffer` (i.e. void ptr + size) arguments.
class BufferDesc {
 public:
  BufferDesc(void* data, size_t size) : data_(data), size_(size) {}
  void* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  void* data_;
  size_t size_;
};

class XlaRuntimeCpuExecutable {
 public:
  explicit XlaRuntimeCpuExecutable(
      std::unique_ptr<xla::runtime::JitExecutable> jit_executable,
      const XlaFrameworkMapping& xla_framework_mapping)
      : jit_executable_(std::move(jit_executable)),
        default_executable_(&jit_executable_->DefaultExecutable().get()),
        xla_framework_mapping_(xla_framework_mapping) {}
  Status Execute(const std::vector<BufferDesc>& descriptor_table);
  xla::runtime::Executable& default_executable() {
    return *default_executable_;
  }

 private:
  std::unique_ptr<xla::runtime::JitExecutable> jit_executable_;
  xla::runtime::Executable* default_executable_;  // owned by jit_executable_.
  XlaFrameworkMapping xla_framework_mapping_;
};

// CPU-targeting implementation of the XLA Executable interface.
//
// Wraps a JIT-ed object that can be executed "on device". We JIT for the host
// architecture, so JIT-ed code and host code share the same ABI.
class CpuExecutable : public Executable {
 public:
  CpuExecutable(std::unique_ptr<SimpleOrcJIT> jit,
                std::unique_ptr<const BufferAssignment> assignment,
                std::unique_ptr<HloModule> hlo_module,
                const std::string& entry_function_name,
                std::unique_ptr<HloProfilePrinterData> hlo_profile_printer_data,
                std::unique_ptr<HloProfileIndexMap> hlo_profile_index_map);
  // XLA Runtime constructor.
  CpuExecutable(
      std::unique_ptr<HloModule> hlo_module,
      std::unique_ptr<HloProfilePrinterData> hlo_profile_printer_data,
      std::unique_ptr<HloProfileIndexMap> hlo_profile_index_map,
      std::unique_ptr<const BufferAssignment> assignment,
      std::unique_ptr<XlaRuntimeCpuExecutable> xla_runtime_executable);

  ~CpuExecutable() override;

  bool IsXlaRuntime() { return xla_runtime_executable_ != nullptr; }

  Status ExecuteXlaRuntime(const std::vector<BufferDesc>& descriptor_table) {
    return xla_runtime_executable_->Execute(descriptor_table);
  }

  StatusOr<ExecutionOutput> ExecuteAsyncOnStream(
      const ServiceExecutableRunOptions* run_options,
      std::vector<ExecutionInput> arguments,
      HloExecutionProfile* hlo_execution_profile) override;

  // Calls the generated function performing the computation with the given
  // arguments using the supplied buffers.
  Status ExecuteComputeFunction(
      const ExecutableRunOptions* run_options,
      absl::Span<MaybeOwningDeviceMemory const> buffers,
      HloExecutionProfile* hlo_execution_profile);

  // This should be called after set_ir_module_string.
  const std::string& ir_module_string() const { return ir_module_string_; }

  void set_ir_module_string(const std::string& ir_module_string) {
    ir_module_string_ = ir_module_string;
  }

  static int64_t ShapeSizeBytes(const Shape& shape);

  // Type of the computation function we expect in the JIT.
  using ComputeFunctionType =
      void (*)(void* /*result*/, const ExecutableRunOptions* /*run_options*/,
               const void** /*args*/, void** /*buffer_table*/,
               XlaCustomCallStatus* /*status*/, int64_t* /*profile_counters*/);

  const ComputeFunctionType& compute_function() const {
    return compute_function_;
  }

  const BufferAssignment& buffer_assignment() const { return *assignment_; }

  int64_t SizeOfGeneratedCodeInBytes() const override;

 private:
  // Creates an array suitable for passing as the "buffer_table" argument to the
  // JIT compiled function pointer.
  //
  // Returns (unowning_buffers, owning_buffers) where:
  //
  //  - unowning_buffers.data() can be passed as the buffer_table argument as-is
  //    and includes pointers to the scratch storage required by the
  //    computation, the live-out buffer into which the result will be written
  //    and entry computation parameters.
  //
  //  - owning_buffers contains owning pointers to the buffers that were
  //    allocated by this routine.  This routine allocates buffers for temporary
  //    storage and the live-out buffer into which the computation writes it
  //    result.
  //
  //  - buffers_to_free: buffers whose ownership was donated by the caller that
  //    are to be freed by the caller.
  StatusOr<std::vector<MaybeOwningDeviceMemory>> CreateBufferTable(
      se::DeviceMemoryAllocator* memory_allocator, int device_ordinal,
      absl::Span<ExecutionInput const> arguments);

  // Creates an Execution output holding ScopedShapedBuffer for holding the
  // result of the computation, moving buffers out of allocated_buffers and into
  // the result as appropriate.  The addresses are set according to buffer
  // assignment.
  StatusOr<ExecutionOutput> CreateResultShapedBuffer(
      const ServiceExecutableRunOptions* run_options,
      absl::Span<MaybeOwningDeviceMemory> buffers,
      absl::Span<ExecutionInput> arguments);

  // Returns the instruction value set of the root instruction of the entry
  // computation. Uses dataflow analysis from buffer assignment.
  const InstructionValueSet& GetRootValueSet() const;

  // The JIT containing compiled modules.
  const std::unique_ptr<SimpleOrcJIT> jit_;

  // Buffer assignment for the buffers we need to allocate.
  const std::unique_ptr<const BufferAssignment> assignment_;

  std::shared_ptr<const BufferAssignmentProto> buffer_assignment_;

  // The LLVM IR, in string format, of the unoptimized module generated for this
  // CpuExecutable. We save a string instead of an llvm::Module* because leaving
  // llvm::Module* in a singleton can cause the heap checker to emit false
  // positives.
  std::string ir_module_string_;

  // Unique identifier.
  std::string module_name_;

  ComputeFunctionType compute_function_;

  // Entry function name for the computation.
  const std::string entry_function_name_;

  // If not null, XLA Runtime is enabled.
  std::unique_ptr<XlaRuntimeCpuExecutable> xla_runtime_executable_;

  CpuExecutable(const CpuExecutable&) = delete;
  CpuExecutable& operator=(const CpuExecutable&) = delete;
};

}  // namespace cpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_CPU_CPU_EXECUTABLE_H_
