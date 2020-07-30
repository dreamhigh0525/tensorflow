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
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/kernels/mlir_generated/tanh_f16_kernel.h"
#include "tensorflow/core/kernels/mlir_generated/tanh_f32_kernel.h"
#include "tensorflow/core/kernels/mlir_generated/tanh_f64_kernel.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/stream_executor.h"

namespace tensorflow {
namespace {
Status CreateKernel(absl::string_view kernel_name, uint64_t num_args,
                    absl::string_view ptx, absl::Span<const uint8_t> cubin_data,
                    se::StreamExecutor* stream_exec,
                    std::unique_ptr<se::KernelBase>& kernel_base) {
  se::MultiKernelLoaderSpec loader_spec(num_args);

  if (!cubin_data.empty()) {
    loader_spec.AddCudaCubinInMemory(
        reinterpret_cast<const char*>(cubin_data.data()), kernel_name);
  }

  kernel_base.reset(new se::KernelBase(stream_exec));
  return stream_exec->GetKernel(loader_spec, kernel_base.get());
}

struct LaunchConfig {
  se::BlockDim blockDim;
  se::ThreadDim threadDim;
};

LaunchConfig GetLaunchConfiguration(std::vector<uint64> tile_sizes,
                                    std::vector<uint64> unrolling_factors,
                                    std::vector<uint64> shape) {
  LaunchConfig result;
  // Ensure the vectors are length 3 and pad with ones.
  tile_sizes.resize(3, 1);
  unrolling_factors.resize(3, 1);
  shape.resize(3, 1);
  // The number of threads is given by the tiling size.
  result.threadDim = se::ThreadDim(tile_sizes[0], tile_sizes[1], tile_sizes[2]);
  // We know that the kernel was generated by mapping the three outer-most
  // dimensions to x,y,z dimensions. So we only need to compute those.
  std::vector<int> block_dims(3);
  for (int i = 0; i < 3; ++i) {
    // Compute the number of grids. We use ceildiv here as we have to allocate
    // an extra thread/block if the division is not even. The kernel contains
    // code to handle the boundaries.
    int number_of_threads =
        (shape[i] + unrolling_factors[i] - 1) / unrolling_factors[i];
    int number_of_grids =
        (number_of_threads + tile_sizes[i] - 1) / tile_sizes[i];
    block_dims[i] = number_of_grids;
  }
  result.blockDim = se::BlockDim(block_dims[0], block_dims[1], block_dims[2]);
  return result;
}

class MlirGeneratedTanhOp : public OpKernel {
 public:
  explicit MlirGeneratedTanhOp(OpKernelConstruction* ctx) : OpKernel(ctx) {}

  void Compute(OpKernelContext* ctx) override {
    auto* stream = ctx->op_device_context()->stream();
    se::KernelBase* kernel;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (!kernel_) {
        OP_REQUIRES_OK(ctx, CreateKernel("Tanh_kernel", 10, "", cubin_data_,
                                         stream->parent(), kernel_));
      }
      kernel = kernel_.get();
    }

    const Tensor& inp = ctx->input(0);
    Tensor* out = nullptr;
    OP_REQUIRES_OK(
        ctx, ctx->forward_input_or_allocate_output({0}, 0, inp.shape(), &out));

    if (inp.NumElements() == 0) {
      return;
    }

    se::KernelArgsArray<10> args;

    args.add_device_memory_argument(
        stream_executor::DeviceMemoryBase(inp.data(), inp.TotalBytes()));
    args.add_device_memory_argument(
        stream_executor::DeviceMemoryBase(inp.data(), inp.TotalBytes()));
    args.add_argument<int64_t>(0);
    args.add_argument<int64_t>(inp.NumElements());
    args.add_argument<int64_t>(1);

    args.add_device_memory_argument(
        stream_executor::DeviceMemoryBase(out->data(), out->TotalBytes()));
    args.add_device_memory_argument(
        stream_executor::DeviceMemoryBase(out->data(), out->TotalBytes()));
    args.add_argument<int64_t>(0);
    args.add_argument<int64_t>(inp.NumElements());
    args.add_argument<int64_t>(1);

    // This has to be aligned with the configuration that was used when
    // generating the kernels. See the corresponding build rules in the `BUILD`
    // file.
    LaunchConfig config = GetLaunchConfiguration(
        {256}, {4}, {static_cast<uint64>(inp.NumElements())});
    OP_REQUIRES_OK(
        ctx, stream->parent()->Launch(stream, config.threadDim, config.blockDim,
                                      *kernel, args));
  }

 protected:
  absl::Span<const uint8_t> cubin_data_;

 private:
  std::unique_ptr<se::KernelBase> kernel_;
  std::mutex mu_;
};

class MlirGeneratedTanhF16Op : public MlirGeneratedTanhOp {
 public:
  explicit MlirGeneratedTanhF16Op(OpKernelConstruction* ctx)
      : MlirGeneratedTanhOp(ctx) {
    cubin_data_ = kTanhF16Kernel;
  }
};

class MlirGeneratedTanhF32Op : public MlirGeneratedTanhOp {
 public:
  explicit MlirGeneratedTanhF32Op(OpKernelConstruction* ctx)
      : MlirGeneratedTanhOp(ctx) {
    cubin_data_ = kTanhF32Kernel;
  }
};

class MlirGeneratedTanhF64Op : public MlirGeneratedTanhOp {
 public:
  explicit MlirGeneratedTanhF64Op(OpKernelConstruction* ctx)
      : MlirGeneratedTanhOp(ctx) {
    cubin_data_ = kTanhF64Kernel;
  }
};
}  // namespace

REGISTER_KERNEL_BUILDER(
    Name("Tanh").Device(DEVICE_GPU).TypeConstraint<Eigen::half>("T"),
    MlirGeneratedTanhF16Op);
REGISTER_KERNEL_BUILDER(
    Name("Tanh").Device(DEVICE_GPU).TypeConstraint<float>("T"),
    MlirGeneratedTanhF32Op);
REGISTER_KERNEL_BUILDER(
    Name("Tanh").Device(DEVICE_GPU).TypeConstraint<double>("T"),
    MlirGeneratedTanhF64Op);
}  // namespace tensorflow
