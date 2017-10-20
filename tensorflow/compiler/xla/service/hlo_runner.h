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
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/stream_executor_no_cuda.h"

namespace xla {

// A base class for running an HloModule. This executes the given HloModule on a
// certain backend directly without using the client interface. HloModule can be
// explicitly built, or loaded from a serialization file (e.g., hlo proto file).
class HloRunner {
 public:
  HloRunner();

  HloRunner(::perftools::gputools::Platform* platform);

  ~HloRunner();

  // Reads the binary proto file in xla.HloProto format, creates and returns the
  // HloModule.
  static StatusOr<std::unique_ptr<HloModule>> ReadModuleFromHloProtoFile(
      const char* filename, const DebugOptions& debug_options);

  // Executes the given module with given literals as input and returns the
  // result as a Literal. The LiteralPtr type accepts Literal* or
  // std::unique_ptr<Literal>.
  template <typename LiteralPtr>
  std::unique_ptr<Literal> Execute(
      std::unique_ptr<HloModule> module,
      const tensorflow::gtl::ArraySlice<LiteralPtr>& literals);

  // Executes the given module and returns a global data handle.
  StatusOr<perftools::gputools::DeviceMemoryBase> Execute(
      std::unique_ptr<HloModule> module,
      tensorflow::gtl::ArraySlice<perftools::gputools::DeviceMemoryBase>
          arguments,
      Shape* result_shape);

  // Transfers the given literal to the device and returns the data handle.
  perftools::gputools::DeviceMemoryBase TransferToDevice(
      const Literal& literal);

  // Transfers the array referred to by the given handle from the device and
  // returns as a Literal.
  std::unique_ptr<Literal> TransferFromDevice(
      const Shape& shape, perftools::gputools::DeviceMemoryBase device_base);

  // Executes the given module and return the result as a Literal.
  std::unique_ptr<Literal> ExecuteAndTransfer(
      std::unique_ptr<HloModule> module,
      tensorflow::gtl::ArraySlice<perftools::gputools::DeviceMemoryBase>
          arguments);

  // If backend is not created in the constructor, creates and returns the
  // default backend. If creation fails, crashes the program.
  //
  // This creates the backend lazily so it's possible to instantiate an
  // HloRunner in a program without any backends linked in.
  Backend& backend();

 private:
  struct EigenThreadPoolWrapper;

  std::vector<perftools::gputools::DeviceMemoryBase> allocations_;

  std::unique_ptr<EigenThreadPoolWrapper> thread_pool_wrapper_;

  std::unique_ptr<Backend> backend_;
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_HLO_RUNNER_H_
