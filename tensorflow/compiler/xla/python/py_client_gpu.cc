/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "absl/base/casts.h"
#include "absl/strings/numbers.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "tensorflow/compiler/xla/python/callback.h"
#include "tensorflow/compiler/xla/python/exceptions.h"
#include "tensorflow/compiler/xla/service/custom_call_target_registry.h"

namespace xla {

extern "C" void XlaPythonGpuCallback(CUstream stream, void** buffers,
                                     const char* opaque, size_t opaque_len,
                                     XlaCustomCallStatus* status) {
  // Ignore `descriptor` arg to callback
  buffers += 1;
  uint64_t descriptor;
  if (!absl::SimpleAtoi(opaque, &descriptor)) {
    throw xla::XlaRuntimeError("Invalid callback descriptor");
    return;
  }
  CpuCallback* callback =
      absl::bit_cast<CpuCallback*>(static_cast<uintptr_t>(descriptor));
  size_t arity = callback->num_args();
  size_t num_results = callback->num_results();
  std::vector<void*> host_input_buffers;
  std::vector<void*> host_output_buffers;
  // Copy input GPU buffers to host
  for (size_t i = 0; i < arity; ++i) {
    CpuCallback::Arg arg = callback->args()[i];
    void* buf = new char[arg.size_in_bytes];
    host_input_buffers.push_back(buf);
    cudaMemcpyAsync(host_input_buffers[i], buffers[i], arg.size_in_bytes,
                    cudaMemcpyDeviceToHost, stream);
  }
  // TODO(sharadmv): we allocate space for host buffers but the callback will
  // return NumPy arrays which wrap host buffers. We could reuse those instead.
  // Allocate space for output buffers on host
  for (size_t i = 0; i < num_results; ++i) {
    CpuCallback::Result result = callback->results()[i];
    void* buf = new char[result.size_in_bytes];
    host_output_buffers.push_back(buf);
  }
  cudaStreamSynchronize(stream);
  void* host_output_buffer = (void*)host_output_buffers.data();
  callback->Call(host_output_buffer, host_input_buffers.data(), status);
  // Copy host output buffers back to device
  for (size_t i = 0; i < num_results; ++i) {
    CpuCallback::Result result = callback->results()[i];
    cudaMemcpyAsync(buffers[arity + i], host_output_buffers[i],
                    result.size_in_bytes, cudaMemcpyHostToDevice, stream);
  }
  // We need to synchronize here to ensure that host buffers are alive while
  // the async copy is happening.
  cudaStreamSynchronize(stream);
  // Free host output buffers
  for (size_t i = 0; i < num_results; ++i) {
    delete static_cast<char*>(host_output_buffers[i]);
  }
  // Free host input buffers
  for (size_t i = 0; i < arity; ++i) {
    delete static_cast<char*>(host_input_buffers[i]);
  }
}

XLA_REGISTER_CUSTOM_CALL_TARGET_WITH_SYM("xla_python_gpu_callback",
                                         &XlaPythonGpuCallback, "CUDA");

}  // namespace xla
