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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_HLO_RUNNER_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_HLO_RUNNER_H_

#include <memory>
#include <string>
#include <vector>

#include "tensorflow/compiler/xla/service/backend.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"

namespace xla {

// A base class for running an HloModule. This executes the given HloModule on a
// certain backend directly without using the client interface. HloModule can be
// explicitly built, or loaded from a serialization file (e.g., hlo proto
// file), or parsed from a hlo textual IR string.
class HloRunner {
 public:
  HloRunner();

  HloRunner(::perftools::gputools::Platform* platform);

  ~HloRunner();

  // Converts an HloModule from the given hlo textual IR string (in
  // HloModule::ToString format).
  static StatusOr<std::unique_ptr<HloModule>> CreateModuleFromString(
      const tensorflow::StringPiece hlo_string,
      const DebugOptions& debug_options);

  // Reads the proto file in xla.HloProto format, creates and returns the
  // HloModule. Will try to parse the filename as binary proto, then try as
  // text proto if that fails.
  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromHloProtoFile(
      const std::string& filename, const DebugOptions& debug_options);

  // Reads the hlo text dump file in HloModule::ToString format, creates and
  // returns the HloModule.
  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromHloTextDumpFile(
      const std::string& filename, const DebugOptions& debug_options);

  // Tries to parse the filename specified first as binary proto format, then
  // as a textual proto format, then textual IR, then gives up if both fail.
  // ReadModuleFromHloProtoFile or ReadModuleFromHloTextDumpFile should be used
  // explicitly when you know the format, this if you don't.
  static StatusOr<std::unique_ptr<HloModule>> ReadModule(
      const std::string& filename, const DebugOptions& debug_options);

  // Executes the given module with given literals as input and returns the
  // result as a Literal. The LiteralPtr type accepts Literal* or
  // std::unique_ptr<Literal>.
  //
  // If run_hlo_passes is false, the module will be executed without Hlo
  // optimization.
  template <typename LiteralPtr>
  StatusOr<std::unique_ptr<Literal>> Execute(
      std::unique_ptr<HloModule> module,
      const tensorflow::gtl::ArraySlice<LiteralPtr> arguments,
      bool run_hlo_passes = true);

  // If backend is not created in the constructor, creates and returns the
  // default backend. If creation fails, crashes the program.
  //
  // This creates the backend lazily so it's possible to instantiate an
  // HloRunner in a program without any backends linked in.
  Backend& backend();

 private:
  StatusOr<std::unique_ptr<Literal>> ExecuteInternal(
      std::unique_ptr<HloModule> module,
      const tensorflow::gtl::ArraySlice<Literal*> arguments,
      bool run_hlo_passes = true);

  struct EigenThreadPoolWrapper;

  std::unique_ptr<EigenThreadPoolWrapper> thread_pool_wrapper_;

  std::unique_ptr<Backend> backend_;
};

template <typename LiteralPtr>
StatusOr<std::unique_ptr<Literal>> HloRunner::Execute(
    std::unique_ptr<HloModule> module,
    const tensorflow::gtl::ArraySlice<LiteralPtr> arguments,
    bool run_hlo_passes) {
  // Construct a vector of plain pointers for the arguments.
  std::vector<Literal*> argument_pointers;
  for (const auto& argument : arguments) {
    argument_pointers.push_back(&*argument);
  }
  return ExecuteInternal(std::move(module), argument_pointers, run_hlo_passes);
}

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_HLO_RUNNER_H_
