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

#ifndef TENSORFLOW_COMPILER_XLA_EXECUTABLE_RUN_OPTIONS_H_
#define TENSORFLOW_COMPILER_XLA_EXECUTABLE_RUN_OPTIONS_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// These classes are forward declared so that ExecutableRunOptions can be linked
// into an XLA-compiled binary without having to link all of the pointed-to
// objects (e.g., for an ahead-of-time compiled CPU binary, the gpu tools don't
// need to be linked).
namespace stream_executor {
class Stream;
class Event;
class Platform;
class DeviceMemoryAllocator;
class DeviceMemoryBase;
}  // namespace stream_executor

namespace Eigen {
struct ThreadPoolDevice;
}  // namespace Eigen

namespace tsl {
class Status;
template <typename T>
class StatusOr;
template <typename T>
class AsyncValueRef;
}  // namespace tsl

namespace xla {

using ::tsl::Status;    // TENSORFLOW_STATUS_OK
using ::tsl::StatusOr;  // TENSORFLOW_STATUS_OK

class DeviceAssignment;
class ExecutionProfile;
class Shape;

namespace gpu {
class GpuExecutableRunOptions;
}  // namespace gpu

// A unique identifier for a particular "logical execution" of an XLA model.
//
// A logical execution might encompass multiple executions of one or more
// HloModules.  Runs that are part of the same logical execution can
// communicate via collective ops (e.g. kAllToAll), whereas runs that are part
// of different logical executions are isolated.
class RunId {
 public:
  // Creates a new, unique RunId.
  RunId();
  explicit RunId(int64_t value) : data_(value) {}

  RunId(const RunId&) = default;
  RunId& operator=(const RunId&) = default;
  friend bool operator==(const RunId& a, const RunId& b);
  std::string ToString() const;
  int64_t ToInt() const;

  template <typename H>
  friend H AbslHashValue(H h, const RunId& id) {
    return H::combine(std::move(h), id.data_);
  }

 private:
  int64_t data_;
};

// Callback used by the GPU backend only. This is an "one-sided" version of
// ThenDoHostCallback that enqueues a callback onto a stream. The difference
// with ThenDoHostCallback is that the device does not block waiting for the
// callback to complete; instead the callback is scheduled by the runtime.
// This functionality must be provided by the caller, and hence is provided in
// callback form.
using ThenExecuteFunction =
    std::function<void(stream_executor::Stream*, std::function<void()>)>;

// Send/Recv operations are asynchronous and can't always report an error to
// the caller synchronously. Send/Recv device memory functions declared below
// return an error immediately if the operation can't be scheduled (e.g. unknown
// channel id), but can return an error via the callback later on if the actual
// data transfer failed. In case of an error the recv buffer will contain
// undefined data (garbage), but it allows the XLA executable to run ahead and
// submit dependent operations to the compute stream. It is the clients
// responsibility to discard computation results if any async data transfer
// errors were reported.
//
// If the caller does not block a thread after submitting work to a stream
// (Stream::BlockHostUntilDone), the error handler callback can potentially
// outlive the execution itself and it should not capture stack allocated
// objects.
using SendRecvErrorHandler = std::function<void(Status)>;

// Callback for sending device buffer to a channel. Returned event will be
// recorded on a `stream` once the send operation is completed and data was
// copied from the `src` memory.
using SendDeviceMemoryFunction =
    std::function<StatusOr<tsl::AsyncValueRef<stream_executor::Event>>(
        int64_t channel_id, stream_executor::Stream* stream, const Shape& shape,
        const stream_executor::DeviceMemoryBase& src,
        SendRecvErrorHandler error_handler)>;

// Callback for receiving device buffer from a channel. Returned event will be
// recorded on a `stream` once the recv operation is completed and data was
// copied into the `dst` memory.
using RecvDeviceMemoryFunction =
    std::function<StatusOr<tsl::AsyncValueRef<stream_executor::Event>>(
        int64_t channel_id, stream_executor::Stream* stream, const Shape& shape,
        stream_executor::DeviceMemoryBase* dst,
        SendRecvErrorHandler error_handler)>;

// Class containing options for running a LocalExecutable.
class ExecutableRunOptions {
 public:
  // Specifies the allocator to use during execution.
  ExecutableRunOptions& set_allocator(
      stream_executor::DeviceMemoryAllocator* allocator);
  stream_executor::DeviceMemoryAllocator* allocator() const;

  // If set, this is the device to run the computation on. Valid device_ordinal
  // values are: 0 to # of devices - 1. These values are identical to the device
  // ordinal values used by StreamExecutor. The device must be of the same type
  // as the executable was compiled for. A value of -1 indicates this option has
  // not been set.
  ExecutableRunOptions& set_device_ordinal(int device_ordinal);
  int device_ordinal() const;

  // If set, this is the stream to run the computation on. The platform of the
  // stream must match the platform the executable was built for.  A value of
  // nullptr indicates the option has not been set.
  ExecutableRunOptions& set_stream(stream_executor::Stream* stream);
  stream_executor::Stream* stream() const;

  // If set, this is the stream to perform host to device transfers on (e.g. any
  // pre-computation transfers). The platform of the stream must match the
  // platform the executable was built for. A value of nullptr indicates the
  // option has not been set.
  ExecutableRunOptions& set_host_to_device_stream(
      stream_executor::Stream* stream);
  stream_executor::Stream* host_to_device_stream() const;

  // If set, this is the stream to perform device to host transfers on.
  // The platform of the stream must match the platform the executable was
  // built for. A value of nullptr indicates the option has not been set.
  ExecutableRunOptions& set_device_to_host_stream(
      stream_executor::Stream* stream);
  stream_executor::Stream* device_to_host_stream() const;

  // Sets the thread pool device on which to run Eigen subcomputations.
  //
  // This field must be set for XLA:CPU models that call Eigen routines, but may
  // be null otherwise.  Routines that use this field should always CHECK (or
  // TF_RET_CHECK) that it's not null before dereferencing it, so that users get
  // a clean crash rather than a segfault.
  //
  // Does not take ownership.
  ExecutableRunOptions& set_intra_op_thread_pool(
      const Eigen::ThreadPoolDevice* intra_op_thread_pool);
  const Eigen::ThreadPoolDevice* intra_op_thread_pool() const;

  // If set, profiling information is written to 'profile'.
  ExecutionProfile* execution_profile() const;
  ExecutableRunOptions& set_execution_profile(ExecutionProfile* profile);

  ExecutableRunOptions& set_device_assignment(
      const DeviceAssignment* device_assignment);
  const DeviceAssignment* device_assignment() const;

  ExecutableRunOptions& set_rng_seed(int rng_seed);
  int rng_seed() const;

  ExecutableRunOptions& set_launch_id(int32_t launch_id) {
    launch_id_ = launch_id;
    return *this;
  }

  int32_t launch_id() const { return launch_id_; }

  ExecutableRunOptions& set_run_id(RunId id);
  RunId run_id() const;

  // See documentation on ThenExecuteFunction.
  ExecutableRunOptions& set_then_execute_function(ThenExecuteFunction* f) {
    then_execute_function_ = f;
    return *this;
  }
  ThenExecuteFunction* then_execute_function() const {
    return then_execute_function_;
  }

  // See documentation on SendDeviceMemoryFunction.
  ExecutableRunOptions& set_send_device_memory_function(
      SendDeviceMemoryFunction* f) {
    send_device_memory_function_ = f;
    return *this;
  }
  SendDeviceMemoryFunction* send_device_memory_function() const {
    return send_device_memory_function_;
  }

  // See documentation on RecvDeviceMemoryFunction.
  ExecutableRunOptions& set_recv_device_memory_function(
      RecvDeviceMemoryFunction* f) {
    recv_device_memory_function_ = f;
    return *this;
  }
  RecvDeviceMemoryFunction* recv_device_memory_function() const {
    return recv_device_memory_function_;
  }

  // GPU-backend specific options. These are kept out-of-line to avoid bloating
  // the size of this dependency for CPU-only AOT builds.
  ExecutableRunOptions& set_gpu_executable_run_options(
      const gpu::GpuExecutableRunOptions* gpu_executable_run_options);
  const gpu::GpuExecutableRunOptions* gpu_executable_run_options() const;

 private:
  stream_executor::DeviceMemoryAllocator* allocator_ = nullptr;
  int device_ordinal_ = -1;
  const DeviceAssignment* device_assignment_ = nullptr;
  stream_executor::Stream* stream_ = nullptr;
  const Eigen::ThreadPoolDevice* intra_op_thread_pool_ = nullptr;
  ExecutionProfile* execution_profile_ = nullptr;
  int rng_seed_ = 0;
  int32_t launch_id_ = 0;
  stream_executor::Stream* device_to_host_stream_ = nullptr;
  stream_executor::Stream* host_to_device_stream_ = nullptr;
  ThenExecuteFunction* then_execute_function_ = nullptr;
  SendDeviceMemoryFunction* send_device_memory_function_ = nullptr;
  RecvDeviceMemoryFunction* recv_device_memory_function_ = nullptr;
  RunId run_id_;
  const gpu::GpuExecutableRunOptions* gpu_executable_run_options_ = nullptr;
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_EXECUTABLE_RUN_OPTIONS_H_
