/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#if defined(ENABLE_ONEDNN_OPENMP) && defined(ENABLE_MKL) && defined(_OPENMP)
#ifndef DNNL_AARCH64_USE_ACL
// Using LLVM's OpenMP header
#include "external/llvm_openmp/include/omp.h"
/* Added EIGEN_DONT_PARALLELIZE to avoid duplicating omp.h, please refer to
this link https://eigen.tuxfamily.org/dox/TopicMultiThreading.html for more
info. It does not have any negative impact on performance. */
#define EIGEN_DONT_PARALLELIZE
#else
#include "omp.h"  // NOLINT
#endif
#endif  // ENABLE_ONEDNN_OPENMP && ENABLE_MKL &&_OPENMP

#include "tensorflow/core/common_runtime/threadpool_device.h"

#include "absl/base/call_once.h"
#include "tensorflow/core/common_runtime/local_device.h"
#include "tensorflow/core/common_runtime/scoped_allocator.h"
#include "tensorflow/core/common_runtime/scoped_allocator_mgr.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/allocator_registry.h"
#include "tensorflow/core/framework/device_base.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/framework/tensor_util.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/graph/types.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/platform/tracing.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/util/util.h"

#ifdef INTEL_MKL
#include "tensorflow/core/common_runtime/mkl_cpu_allocator.h"
#include "tensorflow/core/platform/cpu_info.h"
#endif  // INTEL_MKL

namespace tensorflow {

ThreadPoolDevice::ThreadPoolDevice(const SessionOptions& options,
                                   const string& name, Bytes memory_limit,
                                   const DeviceLocality& locality,
                                   Allocator* allocator)
    : LocalDevice(options, Device::BuildDeviceAttributes(
                               name, DEVICE_CPU, memory_limit, locality)),
      allocator_(allocator),
      scoped_allocator_mgr_(new ScopedAllocatorMgr(name)) {
#if defined(ENABLE_ONEDNN_OPENMP) && defined(INTEL_MKL)
  // Early return when MKL is disabled
  if (!IsMKLEnabled()) return;
#ifdef _OPENMP
  const char* user_omp_threads = getenv("OMP_NUM_THREADS");
  static absl::once_flag num_threads_setting_flag;
  if (user_omp_threads == nullptr) {
    // OMP_NUM_THREADS controls MKL's intra-op parallelization
    // Default to available physical cores
    const int mkl_intra_op = port::NumSchedulableCPUs();
    const int ht = port::NumHyperthreadsPerCore();
    absl::call_once(num_threads_setting_flag, omp_set_num_threads,
                    (mkl_intra_op + ht - 1) / ht);
  }

#ifndef DNNL_AARCH64_USE_ACL
  const char* user_kmp_blocktime = getenv("KMP_BLOCKTIME");
  static absl::once_flag blocktime_setting_flag;
  if (user_kmp_blocktime == nullptr) {
    // Sets the time, in milliseconds, that a thread should wait,
    // after completing the execution of a parallel region, before sleeping.
    absl::call_once(blocktime_setting_flag, kmp_set_blocktime, 1);
  }
#endif

#endif  // _OPENMP
#endif  // defined(ENABLE_ONEDNN_OPENMP) && defined(INTEL_MKL)
}

ThreadPoolDevice::~ThreadPoolDevice() {}

Allocator* ThreadPoolDevice::GetAllocator(AllocatorAttributes attr) {
  return allocator_;
}

Allocator* ThreadPoolDevice::GetScopedAllocator(AllocatorAttributes attr,
                                                int64 step_id) {
  if (attr.scope_id > 0) {
    return scoped_allocator_mgr_->GetContainer(step_id)->GetInstance(
        attr.scope_id);
  }
  LOG(FATAL) << "Unexpected call to ThreadPoolDevice::GetScopedAllocator "
             << "attr.scope_id = " << attr.scope_id;
  return allocator_;
}

Status ThreadPoolDevice::MakeTensorFromProto(
    const TensorProto& tensor_proto, const AllocatorAttributes alloc_attrs,
    Tensor* tensor) {
  if (tensor_proto.dtype() > 0 && tensor_proto.dtype() <= DataType_MAX) {
    Tensor parsed(tensor_proto.dtype());
    if (parsed.FromProto(allocator_, tensor_proto)) {
      *tensor = std::move(parsed);
      return Status::OK();
    }
  }
  return errors::InvalidArgument("Cannot parse tensor from proto: ",
                                 tensor_proto.DebugString());
}

void ThreadPoolDevice::CopyTensorInSameDevice(
    const Tensor* input_tensor, Tensor* output_tensor,
    const DeviceContext* device_context, StatusCallback done) {
  if (input_tensor->NumElements() != output_tensor->NumElements()) {
    done(errors::Internal(
        "CPU->CPU copy shape mismatch: input=", input_tensor->shape(),
        ", output=", output_tensor->shape()));
    return;
  }
  tensor::DeepCopy(*input_tensor, output_tensor);
  done(Status::OK());
}

#ifdef INTEL_MKL
namespace {
class MklCPUAllocatorFactory : public AllocatorFactory {
 public:
  bool NumaEnabled() override { return false; }

  Allocator* CreateAllocator() override { return new MklCPUAllocator; }

  // Note: Ignores numa_node, for now.
  virtual SubAllocator* CreateSubAllocator(int numa_node) {
    return new MklSubAllocator;
  }
};

REGISTER_MEM_ALLOCATOR("MklCPUAllocator", (IsMKLEnabled() ? 200 : 50),
                       MklCPUAllocatorFactory);

}  // namespace
#endif  // INTEL_MKL

}  // namespace tensorflow
